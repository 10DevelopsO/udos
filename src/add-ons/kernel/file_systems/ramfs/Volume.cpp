// Volume.cpp
//
// Copyright (c) 2003, Ingo Weinhold (bonefish@cs.tu-berlin.de)
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program; if not, write to the Free Software
// Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
//
// You can alternatively use *this file* under the terms of the the MIT
// license included in this package.

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vm/vm_page.h>

#include "DebugSupport.h"
#include "Directory.h"
#include "DirectoryEntryTable.h"
#include "Entry.h"
#include "EntryListener.h"
#include "IndexDirectory.h"
#include "Locking.h"
#include "Misc.h"
#include "NameIndex.h"
#include "Node.h"
#include "NodeListener.h"
#include "NodeTable.h"
#include "TwoKeyAVLTree.h"
#include "Volume.h"

// default volume name
static const char *kDefaultVolumeName = "RAMFS";


class NodeListenerGetPrimaryKey {
public:
	inline Node *operator()(const NodeListenerValue &a)
	{
		return a.node;
	}

	inline Node *operator()(const NodeListenerValue &a) const
	{
		return a.node;
	}
};

class NodeListenerGetSecondaryKey {
public:
	inline NodeListener *operator()(const NodeListenerValue &a)
	{
		return a.listener;
	}

	inline NodeListener *operator()(const NodeListenerValue &a) const
	{
		return a.listener;
	}
};

typedef TwoKeyAVLTree<NodeListenerValue, Node*,
					  TwoKeyAVLTreeStandardCompare<Node*>,
					  NodeListenerGetPrimaryKey, NodeListener*,
					  TwoKeyAVLTreeStandardCompare<NodeListener*>,
					  NodeListenerGetSecondaryKey > _NodeListenerTree;
class NodeListenerTree : public _NodeListenerTree {};


class EntryListenerGetPrimaryKey {
public:
	inline Entry *operator()(const EntryListenerValue &a)
	{
		return a.entry;
	}

	inline Entry *operator()(const EntryListenerValue &a) const
	{
		return a.entry;
	}
};


class EntryListenerGetSecondaryKey {
public:
	inline EntryListener *operator()(const EntryListenerValue &a)
	{
		return a.listener;
	}

	inline EntryListener *operator()(const EntryListenerValue &a) const
	{
		return a.listener;
	}
};


typedef TwoKeyAVLTree<EntryListenerValue, Entry*,
					  TwoKeyAVLTreeStandardCompare<Entry*>,
					  EntryListenerGetPrimaryKey, EntryListener*,
					  TwoKeyAVLTreeStandardCompare<EntryListener*>,
					  EntryListenerGetSecondaryKey > _EntryListenerTree;
class EntryListenerTree : public _EntryListenerTree {};


/*!
	\class Volume
	\brief Represents a volume.
*/


Volume::Volume(fs_volume* volume)
	:
	fVolume(volume),
	fName(kDefaultVolumeName),
	fNextNodeID(kRootParentID + 1),
	fNodeTable(NULL),
	fDirectoryEntryTable(NULL),
	fIndexDirectory(NULL),
	fRootDirectory(NULL),
	fNodeListeners(NULL),
	fAnyNodeListeners(),
	fEntryListeners(NULL),
	fAnyEntryListeners(),
	fAccessTime(0),
	fMounted(false)
{
	rw_lock_init(&fLocker, "ramfs volume");
	recursive_lock_init(&fListenersLock, "ramfs listeners");
	recursive_lock_init(&fIteratorLock, "ramfs iterators");
	recursive_lock_init(&fAttributeIteratorLock, "ramfs attribute iterators");
	recursive_lock_init(&fQueryLocker, "ramfs queries");
}


Volume::~Volume()
{
	Unmount();

	recursive_lock_destroy(&fAttributeIteratorLock);
	recursive_lock_destroy(&fIteratorLock);
	recursive_lock_destroy(&fQueryLocker);
	recursive_lock_destroy(&fListenersLock);
	rw_lock_destroy(&fLocker);
}


status_t
Volume::Mount(uint32 flags)
{
	Unmount();

	status_t error = B_OK;
	// create the listener trees
	if (error == B_OK) {
		fNodeListeners = new(nothrow) NodeListenerTree;
		if (!fNodeListeners)
			error = B_NO_MEMORY;
	}
	if (error == B_OK) {
		fEntryListeners = new(nothrow) EntryListenerTree;
		if (!fEntryListeners)
			error = B_NO_MEMORY;
	}
	// create the node table
	if (error == B_OK) {
		fNodeTable = new(nothrow) NodeTable;
		if (fNodeTable)
			error = fNodeTable->InitCheck();
		else
			SET_ERROR(error, B_NO_MEMORY);
	}
	// create the directory entry table
	if (error == B_OK) {
		fDirectoryEntryTable = new(nothrow) DirectoryEntryTable;
		if (fDirectoryEntryTable)
			error = fDirectoryEntryTable->InitCheck();
		else
			SET_ERROR(error, B_NO_MEMORY);
	}
	// create the index directory
	if (error == B_OK) {
		fIndexDirectory = new(nothrow) IndexDirectory(this);
		if (!fIndexDirectory)
			SET_ERROR(error, B_NO_MEMORY);
	}
	// create the root dir
	if (error == B_OK) {
		fRootDirectory = new(nothrow) Directory(this);
		if (fRootDirectory) {
			// set permissions: -rwxr-xr-x
			fRootDirectory->SetMode(
				S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
			error = PublishVNode(fRootDirectory);
		} else
			SET_ERROR(error, B_NO_MEMORY);
	}
	// set mounted flag / cleanup on error
	if (error == B_OK)
		fMounted = true;
	else
		Unmount();
	RETURN_ERROR(error);
}


status_t
Volume::Unmount()
{
	fMounted = false;
	// delete the root directory
	if (fRootDirectory) {
		// deleting the root directory destroys the complete hierarchy
		delete fRootDirectory;
		fRootDirectory = NULL;
	}
	// delete the index directory
	if (fIndexDirectory) {
		delete fIndexDirectory;
		fIndexDirectory = NULL;
	}
	// delete the listener trees
	if (fEntryListeners) {
		delete fEntryListeners;
		fEntryListeners = NULL;
	}
	if (fNodeListeners) {
		delete fNodeListeners;
		fNodeListeners = NULL;
	}
	// delete the tables
	if (fDirectoryEntryTable) {
		delete fDirectoryEntryTable;
		fDirectoryEntryTable = NULL;
	}
	if (fNodeTable) {
		delete fNodeTable;
		fNodeTable = NULL;
	}
	return B_OK;
}


off_t
Volume::CountBlocks() const
{
	// TODO: Compute how many pages we are using across all DataContainers.
	return 0;
}


off_t
Volume::CountFreeBlocks() const
{
	return vm_page_num_free_pages();
}


status_t
Volume::SetName(const char *name)
{
	status_t error = (name ? B_OK : B_BAD_VALUE);
	if (error == B_OK) {
		if (!fName.SetTo(name))
			SET_ERROR(error, B_NO_MEMORY);
	}
	return error;
}


const char *
Volume::GetName() const
{
	return fName.GetString();
}


status_t
Volume::NewVNode(Node *node)
{
	status_t error = NodeAdded(node);
	if (error == B_OK) {
		error = new_vnode(FSVolume(), node->GetID(), node, &gRamFSVnodeOps);
		if (error != B_OK)
			NodeRemoved(node);
	}
	return error;
}


status_t
Volume::PublishVNode(Node *node)
{
	status_t error = NodeAdded(node);
	if (error == B_OK) {
		error = publish_vnode(FSVolume(), node->GetID(), node, &gRamFSVnodeOps,
			node->GetMode(), 0);
		if (error != B_OK)
			NodeRemoved(node);
	}
	return error;
}


status_t
Volume::GetVNode(ino_t id, Node **node)
{
	return (fMounted ? get_vnode(FSVolume(), id, (void**)node) : B_BAD_VALUE);
}


status_t
Volume::GetVNode(Node *node)
{
	Node *dummy = NULL;
	status_t error = (fMounted ? GetVNode(node->GetID(), &dummy)
							   : B_BAD_VALUE );
	if (error == B_OK && dummy != node) {
		FATAL("Two Nodes have the same ID: %" B_PRIdINO "!\n", node->GetID());
		PutVNode(dummy);
		error = B_ERROR;
	}
	return error;
}


status_t
Volume::PutVNode(ino_t id)
{
	return (fMounted ? put_vnode(FSVolume(), id) : B_BAD_VALUE);
}


status_t
Volume::PutVNode(Node *node)
{
	return (fMounted ? put_vnode(FSVolume(), node->GetID()) : B_BAD_VALUE);
}


status_t
Volume::RemoveVNode(Node *node)
{
	if (fMounted)
		return remove_vnode(FSVolume(), node->GetID());

	status_t error = NodeRemoved(node);
	if (error == B_OK)
		delete node;
	return error;
}


status_t
Volume::UnremoveVNode(Node *node)
{
	return (fMounted ? unremove_vnode(FSVolume(), node->GetID()) : B_BAD_VALUE);
}


status_t
Volume::NodeAdded(Node *node)
{
	status_t error = (node ? B_OK : B_BAD_VALUE);
	if (error == B_OK) {
		error = fNodeTable->AddNode(node);
		// notify listeners
		if (error == B_OK) {
			// listeners interested in that node
			NodeListenerTree::Iterator it;
			if (fNodeListeners->FindFirst(node, &it)) {
				for (NodeListenerValue *value = it.GetCurrent();
					 value && value->node == node;
					 value = it.GetNext()) {
					 if (value->flags & NODE_LISTEN_ADDED)
						 value->listener->NodeAdded(node);
				}
			}
			// listeners interested in any node
			int32 count = fAnyNodeListeners.CountItems();
			for (int32 i = 0; i < count; i++) {
				const NodeListenerValue &value = fAnyNodeListeners.ItemAt(i);
				 if (value.flags & NODE_LISTEN_ADDED)
					 value.listener->NodeAdded(node);
			}
		}
	}
	return error;
}


status_t
Volume::NodeRemoved(Node *node)
{
	status_t error = (node ? B_OK : B_BAD_VALUE);
	if (error == B_OK) {
		error = fNodeTable->RemoveNode(node);
		// notify listeners
		if (error == B_OK) {
			// listeners interested in that node
			NodeListenerTree::Iterator it;
			if (fNodeListeners->FindFirst(node, &it)) {
				for (NodeListenerValue *value = it.GetCurrent();
					 value && value->node == node;
					 value = it.GetNext()) {
					 if (value->flags & NODE_LISTEN_REMOVED)
						 value->listener->NodeRemoved(node);
				}
			}
			// listeners interested in any node
			int32 count = fAnyNodeListeners.CountItems();
			for (int32 i = 0; i < count; i++) {
				const NodeListenerValue &value = fAnyNodeListeners.ItemAt(i);
				 if (value.flags & NODE_LISTEN_REMOVED)
					 value.listener->NodeRemoved(node);
			}
		}
	}
	return error;
}


/*!	\brief Finds the node identified by a ino_t.

	\note The method does not initialize the parent ID for non-directory nodes.

	\param id ID of the node to be found.
	\param node pointer to a pre-allocated Node* to be set to the found node.
	\return \c B_OK, if everything went fine.
*/
status_t
Volume::FindNode(ino_t id, Node **node)
{
	status_t error = (node ? B_OK : B_BAD_VALUE);
	if (error == B_OK) {
		*node = fNodeTable->GetNode(id);
		if (!*node)
			error = B_ENTRY_NOT_FOUND;
	}
	return error;
}


status_t
Volume::AddNodeListener(NodeListener *listener, Node *node, uint32 flags)
{
	// check parameters
	if (!listener || (!node && !(flags & NODE_LISTEN_ANY_NODE))
		|| !(flags & NODE_LISTEN_ALL)) {
		return B_BAD_VALUE;
	}

	// add the listener to the right container
	RecursiveLocker locker(fListenersLock);
	status_t error = B_OK;
	NodeListenerValue value(listener, node, flags);
	if (flags & NODE_LISTEN_ANY_NODE) {
		if (!fAnyNodeListeners.AddItem(value))
			error = B_NO_MEMORY;
	} else
		error = fNodeListeners->Insert(value);
	return error;
}


status_t
Volume::RemoveNodeListener(NodeListener *listener, Node *node)
{
	if (!listener)
		return B_BAD_VALUE;

	RecursiveLocker locker(fListenersLock);
	status_t error = B_OK;
	if (node)
		error = fNodeListeners->Remove(node, listener);
	else {
		NodeListenerValue value(listener, node, 0);
		if (!fAnyNodeListeners.RemoveItem(value))
			error = B_ENTRY_NOT_FOUND;
	}
	return error;
}


status_t
Volume::EntryAdded(ino_t id, Entry *entry)
{
	status_t error = (entry ? B_OK : B_BAD_VALUE);
	if (error == B_OK) {
		error = fDirectoryEntryTable->AddEntry(id, entry);
		if (error == B_OK) {
			// notify listeners
			// listeners interested in that entry
			EntryListenerTree::Iterator it;
			if (fEntryListeners->FindFirst(entry, &it)) {
				for (EntryListenerValue *value = it.GetCurrent();
					 value && value->entry == entry;
					 value = it.GetNext()) {
					 if (value->flags & ENTRY_LISTEN_ADDED)
						 value->listener->EntryAdded(entry);
				}
			}
			// listeners interested in any entry
			int32 count = fAnyEntryListeners.CountItems();
			for (int32 i = 0; i < count; i++) {
				const EntryListenerValue &value = fAnyEntryListeners.ItemAt(i);
				 if (value.flags & ENTRY_LISTEN_ADDED)
					 value.listener->EntryAdded(entry);
			}
		}
	}
	return error;
}


status_t
Volume::EntryRemoved(ino_t id, Entry *entry)
{
	status_t error = (entry ? B_OK : B_BAD_VALUE);
	if (error == B_OK) {
		error = fDirectoryEntryTable->RemoveEntry(id, entry);
		if (error == B_OK) {
			// notify listeners
			// listeners interested in that entry
			EntryListenerTree::Iterator it;
			if (fEntryListeners->FindFirst(entry, &it)) {
				for (EntryListenerValue *value = it.GetCurrent();
					 value && value->entry == entry;
					 value = it.GetNext()) {
					 if (value->flags & ENTRY_LISTEN_REMOVED)
						 value->listener->EntryRemoved(entry);
				}
			}
			// listeners interested in any entry
			int32 count = fAnyEntryListeners.CountItems();
			for (int32 i = 0; i < count; i++) {
				const EntryListenerValue &value = fAnyEntryListeners.ItemAt(i);
				 if (value.flags & ENTRY_LISTEN_REMOVED)
					 value.listener->EntryRemoved(entry);
			}
		}
	}
	return error;
}


status_t
Volume::FindEntry(ino_t id, const char *name, Entry **entry)
{
	status_t error = (entry ? B_OK : B_BAD_VALUE);
	if (error == B_OK) {
		*entry = fDirectoryEntryTable->GetEntry(id, name);
		if (!*entry)
			error = B_ENTRY_NOT_FOUND;
	}
	return error;
}


status_t
Volume::AddEntryListener(EntryListener *listener, Entry *entry, uint32 flags)
{
	// check parameters
	if (!listener || (!entry && !(flags & ENTRY_LISTEN_ANY_ENTRY))
		|| !(flags & ENTRY_LISTEN_ALL)) {
		return B_BAD_VALUE;
	}

	// add the listener to the right container
	RecursiveLocker locker(fListenersLock);
	status_t error = B_OK;
	EntryListenerValue value(listener, entry, flags);
	if (flags & ENTRY_LISTEN_ANY_ENTRY) {
		if (!fAnyEntryListeners.AddItem(value))
			error = B_NO_MEMORY;
	} else
		error = fEntryListeners->Insert(value);
	return error;
}


status_t
Volume::RemoveEntryListener(EntryListener *listener, Entry *entry)
{
	if (!listener)
		return B_BAD_VALUE;

	RecursiveLocker locker(fListenersLock);
	status_t error = B_OK;
	if (entry)
		error = fEntryListeners->Remove(entry, listener);
	else {
		EntryListenerValue value(listener, entry, 0);
		if (!fAnyEntryListeners.RemoveItem(value))
			error = B_ENTRY_NOT_FOUND;
	}
	return error;
}


status_t
Volume::NodeAttributeAdded(ino_t id, Attribute *attribute)
{
	status_t error = (attribute ? B_OK : B_BAD_VALUE);
	if (error == B_OK) {
		// notify the respective attribute index
		if (error == B_OK) {
			if (AttributeIndex *index = FindAttributeIndex(
					attribute->GetName(), attribute->GetType())) {
				index->Added(attribute);
			}
		}
	}
	return error;
}


status_t
Volume::NodeAttributeRemoved(ino_t id, Attribute *attribute)
{
	status_t error = (attribute ? B_OK : B_BAD_VALUE);
	if (error == B_OK) {
		// notify the respective attribute index
		if (error == B_OK) {
			if (AttributeIndex *index = FindAttributeIndex(
					attribute->GetName(), attribute->GetType())) {
				index->Removed(attribute);
			}
		}

		// update live queries
		if (error == B_OK && attribute->GetNode()) {
			uint8 oldKey[kMaxIndexKeyLength];
			size_t oldLength;
			attribute->GetKey(oldKey, &oldLength);
			UpdateLiveQueries(NULL, attribute->GetNode(), attribute->GetName(),
				attribute->GetType(), oldKey, oldLength, NULL, 0);
		}
	}
	return error;
}


NameIndex *
Volume::GetNameIndex() const
{
	return (fIndexDirectory ? fIndexDirectory->GetNameIndex() : NULL);
}


LastModifiedIndex *
Volume::GetLastModifiedIndex() const
{
	return (fIndexDirectory ? fIndexDirectory->GetLastModifiedIndex() : NULL);
}


SizeIndex *
Volume::GetSizeIndex() const
{
	return (fIndexDirectory ? fIndexDirectory->GetSizeIndex() : NULL);
}


Index *
Volume::FindIndex(const char *name)
{
	return (fIndexDirectory ? fIndexDirectory->FindIndex(name) : NULL);
}


AttributeIndex *
Volume::FindAttributeIndex(const char *name, uint32 type)
{
	return (fIndexDirectory
			? fIndexDirectory->FindAttributeIndex(name, type) : NULL);
}


void
Volume::AddQuery(Query *query)
{
	RecursiveLocker _(fQueryLocker);

	if (query)
		fQueries.Insert(query);
}


void
Volume::RemoveQuery(Query *query)
{
	RecursiveLocker _(fQueryLocker);

	if (query)
		fQueries.Remove(query);
}


void
Volume::UpdateLiveQueries(Entry *entry, Node* node, const char *attribute,
	int32 type, const uint8 *oldKey, size_t oldLength, const uint8 *newKey,
	size_t newLength)
{
	RecursiveLocker _(fQueryLocker);

	for (Query* query = fQueries.First();
		 query;
		 query = fQueries.GetNext(query)) {
		query->LiveUpdate(entry, node, attribute, type, oldKey, oldLength,
			newKey, newLength);
	}
}


void
Volume::GetAllocationInfo(AllocationInfo &info)
{
	// tables
	info.AddOtherAllocation(sizeof(NodeTable));
	fNodeTable->GetAllocationInfo(info);
	info.AddOtherAllocation(sizeof(DirectoryEntryTable));
	fDirectoryEntryTable->GetAllocationInfo(info);
	// node hierarchy
	fRootDirectory->GetAllocationInfo(info);
	// name
	info.AddStringAllocation(fName.GetLength());
}


bool
Volume::ReadLock()
{
	bool ok = rw_lock_read_lock(&fLocker) == B_OK;
	if (ok && fLocker.owner_count > 1)
		fAccessTime = system_time();
	return ok;
}


void
Volume::ReadUnlock()
{
	rw_lock_read_unlock(&fLocker);
}


bool
Volume::WriteLock()
{
	bool ok = rw_lock_write_lock(&fLocker) == B_OK;
	if (ok && fLocker.owner_count > 1)
		fAccessTime = system_time();
	return ok;
}


void
Volume::WriteUnlock()
{
	rw_lock_write_unlock(&fLocker);
}
