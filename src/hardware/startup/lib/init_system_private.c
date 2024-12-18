/*
 * $QNXLicenseC:
 * Copyright 2008, QNX Software Systems.
 *
 * Licensed under the Apache License, Version 2.0 (the "License"). You
 * may not reproduce, modify or distribute this software except in
 * compliance with the License. You may obtain a copy of the License
 * at: http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" basis,
 * WITHOUT WARRANTIES OF ANY KIND, either express or implied.
 *
 * This file may contain contributions from others, either as
 * contributors under the License or as licensors under other terms.
 * Please review this entire file for other proprietary rights or license
 * notices, as well as the QNX Development Suite License Guide at
 * http://licensing.qnx.com/license-guide/ for other information.
 * $
 */


#include <startup.h>

uintptr_t boot_vaddr_base;
uintptr_t boot_vaddr_end;

static void *
bootstrap_map(uintptr_t vaddr, unsigned size) {
	return startup_memory_map(size, elf_vaddr_to_paddr(vaddr), PROT_READ|PROT_WRITE);
}

static void
load_bootstraps(struct image_header *ifs_hdr,
			struct system_private_entry *private) {
	unsigned					i;
	union image_dirent	 		*dir;
	paddr_t					base_addr;
	uintptr_t					start_vaddr;
	struct ifs_bootstrap_head	*head;
	struct ifs_bootstrap_data	*prev = NULL;

	//
	// Load all the boot images
	//
	dir = (void *)((uint8_t *)ifs_hdr + ifs_hdr->dir_offset);
	i = 0;
	if(!(ifs_hdr->flags & IMAGE_FLAGS_INO_BITS)) {
		crash("Image unsupported - too old");
	}
	for(;;) {
		for( ;; ) {
			if(dir->attr.size == 0) goto done;
			if(dir->attr.ino & IFS_INO_BOOTSTRAP_EXE) break;
			dir = (void *)((uint8_t *)dir + dir->attr.size);
		}
		base_addr = full_image_paddr + shdr->startup_size + dir->file.offset;
		start_vaddr = load_elf(base_addr);
		if(start_vaddr == ~0UL) {
			crash("Unable to load boot process '/%s'\n", dir->file.path);
			/*NOTREACHED*/
		}
		if((rifs_flag & RIFS_FLAG_ENABLE) && rifs_save_elf32data(base_addr, dir, i) == -1) {
			crash("Unable to save boot process elf data\n");
		}
		private = lsp.system_private.p;
		if(prev != NULL) {
			prev->next_entry = start_vaddr;
			startup_memory_unmap(prev);
		} else {
			first_bootstrap_start_vaddr = start_vaddr;
		}
		head = bootstrap_map(start_vaddr-sizeof(*head), sizeof(*head));
		if(head->signature != IFS_BOOTSTRAP_SIGNATURE) {
			crash("missing bootstrap signature");
		}
		prev = bootstrap_map(head->bootstrap, sizeof(*prev));
		if(prev->args != 0) {
			// newer style bootstrap_data
			struct bootargs_entry	*args;

			// Give board specific code a change to modify the
			// command line arguments for this bootstrap executable
			args = bootstrap_map(prev->args, 0x2000);
			tweak_cmdline(args, dir->file.path);
			startup_memory_unmap(args);
		}
		// prev will get unmapped next time around or at end
		startup_memory_unmap(head);
		++i;
		dir = (void *)((uint8_t *)dir + dir->attr.size);
	}
done:
	if(prev == NULL) {
		crash("No bootstrap executables found");
	}
	startup_memory_unmap(prev);
}

void
init_system_private() {
	struct system_private_entry	*private = set_syspage_section(&lsp.system_private, sizeof(*lsp.system_private.p));
	unsigned				opt;
	paddr_t					start_paddr;
	unsigned				mem;
	struct image_header		*ifs_hdr;
	paddr_t					ifs_paddr;
	unsigned				flags_on = 0;
	unsigned				flags_off = 0;
	struct cpuinfo_entry	*cpu;
	unsigned				i;

	// Propagate the -F switches to cpu 0 first.  On x86, if we are disabling PSE
	// we need to do it before calling load_bootstraps(), otherwise it creates 1-1
	// mapped area using large pages, see PR 127992
	optind = 0;
	// need to keep the full option list because getopt() need the list to parse
	// the options properly
	// pass NULL for the option list to ensure that all getopt() calls use the same list of options in main()
	while((opt = getopt(_argc, _argv, NULL)) != -1) {
		unsigned	flags;
		switch(opt) {
		case 'F':
			if(*optarg == '~') {
				flags = strtoul(optarg+1, NULL, 0);
				flags_off |= flags;
				flags_on  &= ~flags;
			} else {
				flags = strtoul(optarg, NULL, 0);
				flags_off &= ~flags;
				flags_on  |= flags;
			}
			break;
		default:
			break;
		}
	}

	lsp.cpuinfo.p[0].flags |= flags_on;
	lsp.cpuinfo.p[0].flags &= ~flags_off;

	if(shdr->flags1 & STARTUP_HDR_FLAGS1_VIRTUAL) {
		private->pagesize = CPU_VIRT_PAGESIZE;
	} else {
		private->pagesize = CPU_PHYS_PAGESIZE;
	}

	//
	// Set the hostname if it hasn't been set yet
	//
	if(find_typed_string(_CS_HOSTNAME) < 0) {
		add_typed_string(_CS_HOSTNAME, "localhost");
	}

	//
	// Get the image file system into it's proper position
	//
	ifs_paddr = full_image_paddr + shdr->startup_size;
	ifs_hdr = MAKE_1TO1_PTR(ifs_paddr);
	// Try and restore the IFS, if enabled
	if(!(rifs_flag & RIFS_FLAG_ENABLE) || rifs_restore_ifs(ifs_paddr) == -1) {
		// Normal (full) load of the IFS
		load_ifs(ifs_paddr);
	}

	//
	// Get the second (non-bootable) image file system into it's proper position
	//
	if(rifs_flag & RIFS_FLAG_IFS2) {
		load_ifs2_nonbootable();
	}

	//
	// Tell Neutrino where the image file system is located
	//
	mem = as_find(AS_NULL_OFF, "memory", NULL);
	as_add(ifs_paddr, ifs_paddr+ifs_hdr->image_size-1, AS_ATTR_ROM,
				"imagefs", mem);

	//
	// Tell Neutrino where the second (non-bootable) image file system is located
	//
	if((rifs_flag & RIFS_FLAG_IFS2)) {
		as_add(ifs2_paddr_dst, ifs2_paddr_dst+ifs2_size-1, AS_ATTR_ROM, "imagefs", mem);
	}

	//
	// Tell Neutrino where startup is (was)
	//
	as_add(full_ram_paddr, full_ram_paddr+shdr->startup_size-1, AS_ATTR_RAM,
				"startup", mem);

	//
	// Tell Neutrino where RAM is reserved for processes in the
	// image file system.
	//
	start_paddr = full_ram_paddr + shdr->startup_size;
	as_add(start_paddr, full_ram_paddr+shdr->ram_size-1, AS_ATTR_RAM,
				"bootram", mem);

	load_bootstraps(ifs_hdr, private);

	// Calculate checksum as last thing we do
	if((rifs_flag & RIFS_FLAG_ENABLE)) {
		rifs_set_cksum(ifs_hdr);
	}

	// Update syspage entries with the given fdt only when -u startup option is specified
	if (fdt_boot_option && (fdt != NULL)) {
		as_add(fdt_paddr, fdt_paddr + fdt_size - 1, AS_ATTR_ROM, "fdt", mem);
		alloc_ram(fdt_paddr, fdt_size, 1);
	}

	//
	// Add the additional memory blocks specified by the -M option(s) now.
	//
	optind = 0;
	// pass NULL for the option list to ensure that all getopt() calls use the same list of options in main()
	while((opt = getopt(_argc, _argv, NULL)) != -1) {
		char		*p;
		paddr_t		start;
		size_t		size;
		paddr_t		resmem_addr;
		size_t		resmem_size;
		uint8_t 	resmem_flag = 0;
		paddr_t		jtag_syspage_addr;

		switch(opt) {
		case 'A':
			private->private_flags |= SYSTEM_PRIVATE_FLAG_ABNORMAL_REBOOT;
			break;
		case 'M':
#if __PTR_BITS__ != 64
			start = getsize(optarg, &p);
			if(*p != ',') {
				crash("missing comma in -M option near '%s'.\n", p);
			}
			size = getsize(p + 1, &p);
			unsigned type = MEMTYPE_RAM;
			if(*p == ',') {
				type = strtoul(p + 1, NULL, 0);
			}
			if(type == MEMTYPE_RAM) {
				add_ram(start, size);
			} else {
				kprintf("-M option memory type %d is no longer supported - ignored\n", type);
			}
#else
			start = getsize(optarg, &p);
			if(*p != ',') {
				crash("missing comma in -M option near '%s'.\n", p);
			}
			size = getsize(p + 1, &p);
			if(*p == ',') {
				kprintf("-M option: memory types not supported - ignored (%s)\n", p+1);
			}
			add_ram(start, size);
#endif
			break;
		case 'r':
			// reserve user-specified memory from asinfo
			resmem_addr = getsize(optarg, &p);
			if(*p == ',') {
				resmem_size = getsize(p + 1, &p);

				// user can choose to not clear reserved memory
				if(*p == ',') {
					resmem_flag = strtoul(p + 1, &p, 2);
				}
				jtag_reserve_memory(resmem_addr, resmem_size, resmem_flag);
			}
			// else: ignore the option - a size is required
			break;
		case 'j':
			// save syspage address to be read by a JTAG
			jtag_syspage_addr = getsize(optarg, &p);
			jtag_reserve_syspage_addr(jtag_syspage_addr);
			break;
		case 'Z':
			lsp.qtime.p->flags |= QTIME_FLAG_TICKLESS;
			break;
		}
	}

	//
	// If the user wanted to reserve some memory, allocate it now.
	// Unless specified, get memory from the top of available memory
	// so that we use as little of any one-to-one mapping area
	// as possible.
	//

	while (reserved_ram_list != NULL) {
		if(reserved_ram_list->size > 0) {
			reserve_ram(reserved_ram_list->size, reserved_ram_list->align,reserved_ram_list->name, private->pagesize, reserved_ram_list->low_region);
		}
		reserved_ram_list = reserved_ram_list->next;
	}

	// Add memory allocation specific to hardware mitigation
	init_mitigation_mem();

	//
	// Start any AP's.
	//
	smp_hook_rtn();

	// Propagate the -F switches. Has to be done down here because the
	// smp_hook_rtn() call just above is the one that sets the flags
	// field for the AP's.
	cpu = lsp.cpuinfo.p;
	for(i = 0; i < lsp.syspage.p->num_cpu; ++i, ++cpu) {
		cpu->flags |= flags_on;
		cpu->flags &= ~flags_off;
	}

	//
	// Add the asinfo "sysram" entries.
	//
	add_sysram();

	hypervisor_syspage_init();

	//
	// Allocate the real storage location for the system & cpu pages.
	// We can't call set_syspage_section()/grow_syspage_section() after this.
	//
	alloc_syspage_memory();

	// Save the address of the system page to be retrieved by a JTAG.
	// NOTE: Must be stored after alloc_syspage_memory.
	jtag_store_syspage_addr();
}
