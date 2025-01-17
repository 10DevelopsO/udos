/*
 * Copyright 2004-2018, Haiku Inc. All Rights Reserved.
 * Distributed under the terms of the MIT license.
 *
 * Copyright 2002, Travis Geiselbrecht. All rights reserved.
 * Distributed under the terms of the NewOS License.
 */


#ifdef _BOOT_MODE
#	include <boot/arch.h>
#endif

#include <KernelExport.h>

#include <elf_priv.h>
#include <arch/elf.h>


//#define TRACE_ARCH_ELF
#ifdef TRACE_ARCH_ELF
#	define TRACE(x) dprintf x
#else
#	define TRACE(x) ;
#endif


#ifndef _BOOT_MODE
static bool
is_in_image(struct elf_image_info *image, addr_t address)
{
	return (address >= image->text_region.start
			&& address < image->text_region.start + image->text_region.size)
		|| (address >= image->data_region.start
			&& address < image->data_region.start + image->data_region.size);
}
#endif	// !_BOOT_MODE


#if !defined(__x86_64__) || defined(ELF32_COMPAT)	\
	 || (defined(_BOOT_MODE) && _BOOT_PLATFORM != efi)


#ifdef TRACE_ARCH_ELF
static const char *kRelocations[] = {
	"R_386_NONE",
	"R_386_32",			/* add symbol value */
	"R_386_PC32",		/* add PC relative symbol value */
	"R_386_GOT32",		/* add PC relative GOT offset */
	"R_386_PLT32",		/* add PC relative PLT offset */
	"R_386_COPY",		/* copy data from shared object */
	"R_386_GLOB_DAT",	/* set GOT entry to data address */
	"R_386_JMP_SLOT",	/* set GOT entry to code address */
	"R_386_RELATIVE",	/* add load address of shared object */
	"R_386_GOTOFF",		/* add GOT relative symbol address */
	"R_386_GOTPC",		/* add PC relative GOT table address */
};
#endif


#ifdef _BOOT_MODE
status_t
boot_arch_elf_relocate_rel(struct preloaded_elf32_image *image, Elf32_Rel *rel,
	int relLength)
#else
int
arch_elf_relocate_rel(struct elf_image_info *image,
	struct elf_image_info *resolveImage, Elf32_Rel *rel, int relLength)
#endif
{
	elf_addr S;
	uint32 A;
	uint32 P;
	uint32 finalAddress;
	uint32 *resolveAddress;
	int i;

	S = A = P = 0;

	for (i = 0; i * (int)sizeof(Elf32_Rel) < relLength; i++) {
		TRACE(("looking at rel type %s, offset 0x%lx\n",
			kRelocations[ELF32_R_TYPE(rel[i].r_info)], rel[i].r_offset));

		// calc S
		switch (ELF32_R_TYPE(rel[i].r_info)) {
			case R_386_32:
			case R_386_PC32:
			case R_386_GLOB_DAT:
			case R_386_JMP_SLOT:
			case R_386_GOTOFF:
			{
				Elf32_Sym *symbol;
				status_t status;

				symbol = SYMBOL(image, ELF32_R_SYM(rel[i].r_info));

#ifdef _BOOT_MODE
				status = boot_elf_resolve_symbol(image, symbol, &S);
#else
				status = elf_resolve_symbol(image, symbol, resolveImage, &S);
#endif
				if (status < B_OK)
					return status;
				TRACE(("S %p (%s)\n", (void *)S, SYMNAME(image, symbol)));
			}
		}
		// calc A
		switch (ELF32_R_TYPE(rel[i].r_info)) {
			case R_386_32:
			case R_386_PC32:
			case R_386_GOT32:
			case R_386_PLT32:
			case R_386_RELATIVE:
			case R_386_GOTOFF:
			case R_386_GOTPC:
				A = *(uint32 *)(image->text_region.delta + rel[i].r_offset);
				TRACE(("A %p\n", (void *)A));
				break;
		}
		// calc P
		switch (ELF32_R_TYPE(rel[i].r_info)) {
			case R_386_PC32:
			case R_386_GOT32:
			case R_386_PLT32:
			case R_386_GOTPC:
				P = image->text_region.delta + rel[i].r_offset;
				TRACE(("P %p\n", (void *)P));
				break;
		}

		switch (ELF32_R_TYPE(rel[i].r_info)) {
			case R_386_NONE:
				continue;
			case R_386_32:
				finalAddress = S + A;
				break;
			case R_386_PC32:
				finalAddress = S + A - P;
				break;
			case R_386_RELATIVE:
				// B + A;
				finalAddress = image->text_region.delta + A;
				break;
			case R_386_JMP_SLOT:
			case R_386_GLOB_DAT:
				finalAddress = S;
				break;

			default:
				dprintf("arch_elf_relocate_rel: unhandled relocation type %d\n",
					ELF32_R_TYPE(rel[i].r_info));
				return B_BAD_DATA;
		}

		resolveAddress = (uint32 *)(image->text_region.delta + rel[i].r_offset);
#ifndef _BOOT_MODE
		if (!is_in_image(image, (addr_t)resolveAddress)) {
			dprintf("arch_elf_relocate_rel: invalid offset %#lx\n",
				rel[i].r_offset);
			return B_BAD_ADDRESS;
		}
#endif
		*resolveAddress = finalAddress;
		TRACE(("-> offset %#lx (%#lx) = %#lx\n",
			(image->text_region.delta + rel[i].r_offset), rel[i].r_offset, finalAddress));
	}

	return B_NO_ERROR;
}


#ifdef _BOOT_MODE
status_t
boot_arch_elf_relocate_rela(struct preloaded_elf32_image *image,
	Elf32_Rela *rel, int relLength)
#else
int
arch_elf_relocate_rela(struct elf_image_info *image,
	struct elf_image_info *resolveImage, Elf32_Rela *rel, int relLength)
#endif
{
	dprintf("arch_elf_relocate_rela: not supported on x86\n");
	return B_ERROR;
}


#endif	// !defined(__x86_64__) || defined(ELF32_COMPAT) ||
	// (defined(_BOOT_MODE) && _BOOT_PLATFORM != efi)


#if (defined(__x86_64__) && !defined(ELF32_COMPAT)) || \
	(defined(_BOOT_MODE) && defined(BOOT_SUPPORT_ELF64))


#ifdef _BOOT_MODE
status_t
boot_arch_elf_relocate_rel(preloaded_elf64_image* image, Elf64_Rel* rel,
	int relLength)
#else
int
arch_elf_relocate_rel(struct elf_image_info *image,
	struct elf_image_info *resolveImage, Elf64_Rel *rel, int relLength)
#endif
{
	dprintf("arch_elf_relocate_rel: not supported on x86_64\n");
	return B_ERROR;
}


#ifdef _BOOT_MODE
status_t
boot_arch_elf_relocate_rela(preloaded_elf64_image* image, Elf64_Rela* rel,
	int relLength)
#else
int
arch_elf_relocate_rela(struct elf_image_info *image,
	struct elf_image_info *resolveImage, Elf64_Rela *rel, int relLength)
#endif
{
	for (int i = 0; i < relLength / (int)sizeof(Elf64_Rela); i++) {
		int type = ELF64_R_TYPE(rel[i].r_info);
		int symIndex = ELF64_R_SYM(rel[i].r_info);
		Elf64_Addr symAddr = 0;

		// Resolve the symbol, if any.
		if (symIndex != 0) {
			Elf64_Sym* symbol = SYMBOL(image, symIndex);

			status_t status;
#ifdef _BOOT_MODE
			status = boot_elf_resolve_symbol(image, symbol, &symAddr);
#else
			status = elf_resolve_symbol(image, symbol, resolveImage, &symAddr);
#endif
			if (status < B_OK)
				return status;
		}

		// Address of the relocation.
		Elf64_Addr relocAddr = image->text_region.delta + rel[i].r_offset;

		// Calculate the relocation value.
		Elf64_Addr relocValue;
		switch (type) {
			case R_X86_64_NONE:
				continue;
			case R_X86_64_64:
				relocValue = symAddr + rel[i].r_addend;
				break;
			case R_X86_64_PC32:
				relocValue = symAddr + rel[i].r_addend - rel[i].r_offset;
				break;
			case R_X86_64_GLOB_DAT:
			case R_X86_64_JUMP_SLOT:
				relocValue = symAddr + rel[i].r_addend;
				break;
			case R_X86_64_RELATIVE:
				relocValue = image->text_region.delta + rel[i].r_addend;
				break;
			default:
				dprintf("arch_elf_relocate_rela: unhandled relocation type %d\n",
					type);
				return B_BAD_DATA;
		}
#ifdef _BOOT_MODE
		boot_elf64_set_relocation(relocAddr, relocValue);
#else
		if (!is_in_image(image, relocAddr)) {
			dprintf("arch_elf_relocate_rela: invalid offset %#lx\n",
				rel[i].r_offset);
			return B_BAD_ADDRESS;
		}

		if (type == R_X86_64_PC32)
			*(Elf32_Addr *)relocAddr = relocValue;
		else
			*(Elf64_Addr *)relocAddr = relocValue;
#endif
	}

	return B_OK;
}


#endif	// (defined(__x86_64__) && !defined(ELF32_COMPAT)) ||
	// (defined(_BOOT_MODE) && defined(BOOT_SUPPORT_ELF64))
