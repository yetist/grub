/* linux.c - boot Linux */
/*
 *  GRUB  --  GRand Unified Bootloader
 *  Copyright (C) 2003,2004,2005,2007,2009,2010,2017  Free Software Foundation, Inc.
 *
 *  GRUB is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  GRUB is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with GRUB.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <grub/efi/api.h>
#include <grub/efi/efi.h>
#include <grub/elf.h>
#include <grub/elfload.h>
#include <grub/loader.h>
#include <grub/dl.h>
#include <grub/mm.h>
#include <grub/misc.h>
#include <grub/command.h>
#include <grub/cpu/relocator.h>
#include <grub/memory.h>
#include <grub/i18n.h>
#include <grub/lib/cmdline.h>
#include <grub/linux.h>

GRUB_MOD_LICENSE ("GPLv3+");

#pragma GCC diagnostic ignored "-Wcast-align"

static grub_dl_t my_mod;

static int loaded;

static grub_size_t linux_size;

static struct grub_relocator *relocator;
static grub_addr_t target_addr, entry_addr;
static int linux_argc;
static grub_uint8_t *linux_args_addr;
static grub_off_t rd_addr_arg_off, rd_size_arg_off;
static int initrd_loaded = 0;

static grub_err_t
grub_linux_boot (void)
{
  struct grub_relocator64_state state;

  grub_memset (&state, 0, sizeof (state));

  /* Boot the kernel.  */
  state.gpr[1] = entry_addr;

  state.gpr[4] = linux_argc;
  state.gpr[5] = (grub_addr_t) linux_args_addr;
  state.jumpreg = 1;
  grub_relocator64_boot (relocator, state);

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_linux_unload (void)
{
  grub_relocator_unload (relocator);
  grub_dl_unref (my_mod);

  loaded = 0;

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_linux_load32 (grub_elf_t elf, const char *filename)
{
  Elf32_Addr base;
  grub_err_t err;
  grub_uint8_t *playground;

  /* Linux's entry point incorrectly contains a virtual address.  */
  entry_addr = elf->ehdr.ehdr32.e_entry;

  linux_size = grub_elf32_size (elf, &base, 0);
  if (linux_size == 0)
    return grub_errno;
  target_addr = base;
  linux_size = ALIGN_UP (base + linux_size - base, 8);

  relocator = grub_relocator_new ();
  if (!relocator)
    return grub_errno;

  {
    grub_relocator_chunk_t ch;
    err = grub_relocator_alloc_chunk_addr (relocator, &ch,
					   grub_vtop ((void *) target_addr),
					   linux_size);
    if (err)
      return err;
    playground = get_virtual_current_address (ch);
  }

  /* Now load the segments into the area we claimed.  */
  return grub_elf32_load (elf, filename, playground - base, GRUB_ELF_LOAD_FLAGS_NONE, 0, 0);
}

static grub_err_t
grub_linux_load64 (grub_elf_t elf, const char *filename)
{
  Elf64_Addr base;
  grub_err_t err;
  grub_uint8_t *playground;

  /* Linux's entry point incorrectly contains a virtual address.  */
  entry_addr = elf->ehdr.ehdr64.e_entry;

  linux_size = grub_elf64_size (elf, &base, 0);
  if (linux_size == 0)
    return grub_errno;
  target_addr = base;
  linux_size = ALIGN_UP (base + linux_size - base, 8);

  relocator = grub_relocator_new ();
  if (!relocator)
    return grub_errno;

  {
    grub_relocator_chunk_t ch;
    err = grub_relocator_alloc_chunk_addr (relocator, &ch,
					   grub_vtop ((void *) target_addr),
					   linux_size);
    if (err)
      return err;
    playground = get_virtual_current_address (ch);
  }

  /* Now load the segments into the area we claimed.  */
  return grub_elf64_load (elf, filename, playground - base, GRUB_ELF_LOAD_FLAGS_NONE, 0, 0);
}

static grub_err_t
grub_cmd_linux (grub_command_t cmd __attribute__ ((unused)),
		int argc, char *argv[])
{
  grub_elf_t elf = 0;
  int size;
  int i;
  grub_uint32_t *linux_argv;
  char *linux_args;
  grub_err_t err;

  if (argc == 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));

  elf = grub_elf_open (argv[0]);
  if (! elf)
    return grub_errno;

  if (elf->ehdr.ehdr32.e_type != ET_EXEC)
    {
      grub_elf_close (elf);
      return grub_error (GRUB_ERR_UNKNOWN_OS,
			 N_("this ELF file is not of the right type"));
    }

  /* Release the previously used memory.  */
  grub_loader_unset ();
  loaded = 0;

  /* For arguments.  */
  linux_argc = argc;
  /* Main arguments.  */
  size = (linux_argc) * sizeof (grub_uint32_t);
  /* Initrd address and size.  */
  size += 2 * sizeof (grub_uint32_t);
  /* NULL terminator.  */
  size += sizeof (grub_uint32_t);

  /* First argument is always "a0".  */
  size += ALIGN_UP (sizeof ("a0"), 4);
  /* Normal arguments.  */
  for (i = 1; i < argc; i++)
    size += ALIGN_UP (grub_strlen (argv[i]) + 1, 4);

  /* rd arguments.  */
  size += ALIGN_UP (sizeof ("rd_start=0xXXXXXXXXXXXXXXXX"), 4);
  size += ALIGN_UP (sizeof ("rd_size=0xXXXXXXXXXXXXXXXX"), 4);

  size = ALIGN_UP (size, 8);

  if (grub_elf_is_elf32 (elf))
    err = grub_linux_load32 (elf, argv[0]);
  else
  if (grub_elf_is_elf64 (elf))
    err = grub_linux_load64 (elf, argv[0]);
  else
    err = grub_error (GRUB_ERR_BAD_OS, N_("invalid arch-dependent ELF magic"));

  grub_elf_close (elf);

  if (err)
    return err;

  {
    grub_relocator_chunk_t ch;
    err = grub_relocator_alloc_chunk_align (relocator, &ch,
					    0, (0xffffffff - size) + 1,
					    size, 8,
					    GRUB_RELOCATOR_PREFERENCE_HIGH, 0);
    if (err)
      return err;
    linux_args_addr = get_virtual_current_address (ch);
  }

  linux_argv = (grub_uint32_t *) linux_args_addr;
  linux_args = (char *) (linux_argv + (linux_argc + 1 + 2));

  grub_memcpy (linux_args, "a0", sizeof ("a0"));
  *linux_argv = (grub_uint32_t) (grub_addr_t) linux_args;
  linux_argv++;
  linux_args += ALIGN_UP (sizeof ("a0"), 4);

  for (i = 1; i < argc; i++)
    {
      grub_memcpy (linux_args, argv[i], grub_strlen (argv[i]) + 1);
      *linux_argv = (grub_uint32_t) (grub_addr_t) linux_args;
      linux_argv++;
      linux_args += ALIGN_UP (grub_strlen (argv[i]) + 1, 4);
    }

  /* Reserve space for rd arguments.  */
  rd_addr_arg_off = (grub_uint8_t *) linux_args - linux_args_addr;
  linux_args += ALIGN_UP (sizeof ("rd_start=0xXXXXXXXXXXXXXXXX"), 4);
  *linux_argv = 0;
  linux_argv++;

  rd_size_arg_off = (grub_uint8_t *) linux_args - linux_args_addr;
  linux_args += ALIGN_UP (sizeof ("rd_size=0xXXXXXXXXXXXXXXXX"), 4);
  *linux_argv = 0;
  linux_argv++;

  *linux_argv = 0;

  grub_loader_set (grub_linux_boot, grub_linux_unload, GRUB_LOADER_FLAG_NORETURN);
  initrd_loaded = 0;
  loaded = 1;
  grub_dl_ref (my_mod);

  return GRUB_ERR_NONE;
}

static grub_err_t
grub_cmd_initrd (grub_command_t cmd __attribute__ ((unused)),
		 int argc, char *argv[])
{
  grub_size_t size = 0;
  void *initrd_dest;
  grub_err_t err;
  struct grub_linux_initrd_context initrd_ctx = { 0, 0, 0 };

  if (argc == 0)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("filename expected"));

  if (!loaded)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, N_("you need to load the kernel first"));

  if (initrd_loaded)
    return grub_error (GRUB_ERR_BAD_ARGUMENT, "only one initrd command can be issued.");

  if (grub_initrd_init (argc, argv, &initrd_ctx))
    goto fail;

  size = grub_get_initrd_size (&initrd_ctx);

  {
    grub_relocator_chunk_t ch;
    err = grub_relocator_alloc_chunk_align (relocator, &ch,
					    0, (0xffffffff - size) + 1,
					    size, 0x10000,
					    GRUB_RELOCATOR_PREFERENCE_HIGH, 0);

    if (err)
      goto fail;
    initrd_dest = get_virtual_current_address (ch);
  }

  if (grub_initrd_load (&initrd_ctx, argv, initrd_dest))
    goto fail;

  grub_snprintf ((char *) linux_args_addr + rd_addr_arg_off,
		 sizeof ("rd_start=0xXXXXXXXXXXXXXXXX"), "rd_start=0x%lx",
		(grub_uint64_t) initrd_dest);
  ((grub_uint32_t *) linux_args_addr)[linux_argc]
    = (grub_uint32_t) ((grub_addr_t) linux_args_addr + rd_addr_arg_off);
  linux_argc++;

  grub_snprintf ((char *) linux_args_addr + rd_size_arg_off,
		sizeof ("rd_size=0xXXXXXXXXXXXXXXXXX"), "rd_size=0x%lx",
		(grub_uint64_t) size);
  ((grub_uint32_t *) linux_args_addr)[linux_argc]
    = (grub_uint32_t) ((grub_addr_t) linux_args_addr + rd_size_arg_off);
  linux_argc++;

  initrd_loaded = 1;

 fail:
  grub_initrd_close (&initrd_ctx);

  return grub_errno;
}

static grub_command_t cmd_linux, cmd_initrd;

GRUB_MOD_INIT(linux)
{
  cmd_linux = grub_register_command ("linux", grub_cmd_linux,
				     0, N_("Load Linux."));
  cmd_initrd = grub_register_command ("initrd", grub_cmd_initrd,
				      0, N_("Load initrd."));
  my_mod = mod;
}

GRUB_MOD_FINI(linux)
{
  grub_unregister_command (cmd_linux);
  grub_unregister_command (cmd_initrd);
}
