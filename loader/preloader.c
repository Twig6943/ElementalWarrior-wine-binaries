/*
 * Preloader for ld.so
 *
 * Copyright (C) 1995,96,97,98,99,2000,2001,2002 Free Software Foundation, Inc.
 * Copyright (C) 2004 Mike McCormack for Codeweavers
 * Copyright (C) 2004 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * Design notes
 *
 * The goal of this program is to be a workaround for exec-shield, as used
 *  by the Linux kernel distributed with Fedora Core and other distros.
 *
 * To do this, we implement our own shared object loader that reserves memory
 * that is important to Wine, and then loads the main binary and its ELF
 * interpreter.
 *
 * We will try to set up the stack and memory area so that the program that
 * loads after us (eg. the wine binary) never knows we were here, except that
 * areas of memory it needs are already magically reserved.
 *
 * The following memory areas are important to Wine:
 *  0x00000000 - 0x00110000  the DOS area
 *  0x80000000 - 0x81000000  the shared heap
 *  ???        - ???         the PE binary load address (usually starting at 0x00400000)
 *
 * If this program is used as the shared object loader, the only difference
 * that the loaded programs should see is that this loader will be mapped
 * into memory when it starts.
 */

/*
 * References (things I consulted to understand how ELF loading works):
 *
 * glibc 2.3.2   elf/dl-load.c
 *  http://www.gnu.org/directory/glibc.html
 *
 * Linux 2.6.4   fs/binfmt_elf.c
 *  ftp://ftp.kernel.org/pub/linux/kernel/v2.6/linux-2.6.4.tar.bz2
 *
 * Userland exec, by <grugq@hcunix.net>
 *  http://cert.uni-stuttgart.de/archive/bugtraq/2004/01/msg00002.html
 *
 * The ELF specification:
 *  http://www.linuxbase.org/spec/booksets/LSB-Embedded/LSB-Embedded/book387.html
 */

#include "config.h"
#include "wine/port.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef HAVE_SYS_MMAN_H
# include <sys/mman.h>
#endif
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_ELF_H
# include <elf.h>
#endif
#ifdef HAVE_LINK_H
# include <link.h>
#endif
#ifdef HAVE_SYS_LINK_H
# include <sys/link.h>
#endif

#include "main.h"

/* ELF definitions */
#define ELF_PREFERRED_ADDRESS(loader, maplength, mapstartpref) (mapstartpref)
#define ELF_FIXED_ADDRESS(loader, mapstart) ((void) 0)

#define MAP_BASE_ADDR(l)     0

#ifndef MAP_COPY
#define MAP_COPY MAP_PRIVATE
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

static struct wine_preload_info preload_info[] =
{
    { (void *)0x00000000, 0x00110000 },  /* DOS area */
    { (void *)0x80000000, 0x01000000 },  /* shared heap */
    { (void *)0x00110000, 0x0fef0000 },  /* default PE exe range (may be set with WINEPRELOADRESERVE) */
    { 0, 0 }                             /* end of list */
};

/* debugging */
#undef DUMP_SEGMENTS
#undef DUMP_AUX_INFO
#undef DUMP_SYMS

/* older systems may not define these */
#ifndef PT_TLS
#define PT_TLS 7
#endif

static unsigned int page_size, page_mask;
static char *preloader_start, *preloader_end;

struct wld_link_map {
    ElfW(Addr) l_addr;
    ElfW(Dyn) *l_ld;
    const ElfW(Phdr) *l_phdr;
    ElfW(Addr) l_entry;
    ElfW(Half) l_ldnum;
    ElfW(Half) l_phnum;
    ElfW(Addr) l_map_start, l_map_end;
    ElfW(Addr) l_interp;
};


/*
 * The _start function is the entry and exit point of this program
 *
 *  It calls wld_start, passing a pointer to the args it receives
 *  then jumps to the address wld_start returns after removing the
 *  first argv[] value, and decrementing argc
 */
void _start();
extern char _end[];
__ASM_GLOBAL_FUNC(_start,
                  "\tcall wld_start\n"
                  "\tpush %eax\n"
                  "\txor %eax,%eax\n"
                  "\txor %ecx,%ecx\n"
                  "\txor %edx,%edx\n"
                  "\tret\n")

/*
 * wld_printf - just the basics
 *
 *  %x prints a hex number
 *  %s prints a string
 */
static void wld_vsprintf(char *str, const char *fmt, va_list args )
{
    static const char hex_chars[16] = "0123456789abcdef";
    const char *p = fmt;

    while( *p )
    {
        if( *p == '%' )
        {
            p++;
            if( *p == 'x' )
            {
                int i;
                unsigned int x = va_arg( args, unsigned int );
                for(i=7; i>=0; i--)
                    *str++ = hex_chars[(x>>(i*4))&0xf];
            }
            else if( *p == 's' )
            {
                char *s = va_arg( args, char * );
                while(*s)
                    *str++ = *s++;
            }
            else if( *p == 0 )
                break;
            p++;
        }
        *str++ = *p++;
    }
    *str = 0;
}

static void wld_printf(const char *fmt, ... )
{
    va_list args;
    char buffer[256];

    va_start( args, fmt );
    wld_vsprintf(buffer, fmt, args );
    va_end( args );
    write(2, buffer, strlen(buffer));
}

static void fatal_error(const char *fmt, ... )
{
    va_list args;
    char buffer[256];

    va_start( args, fmt );
    wld_vsprintf(buffer, fmt, args );
    va_end( args );
    write(2, buffer, strlen(buffer));
    _exit(1);
}

#ifdef DUMP_AUX_INFO
/*
 *  Dump interesting bits of the ELF auxv_t structure that is passed
 *   as the 4th parameter to the _start function
 */
static void dump_auxiliary( ElfW(auxv_t) *av )
{
  for (  ; av->a_type != AT_NULL; av++)
    switch (av->a_type)
      {
      case AT_PAGESZ:
        wld_printf("AT_PAGESZ = %x\n",av->a_un.a_val);
        break;
      case AT_PHDR:
        wld_printf("AT_PHDR = %x\n",av->a_un.a_ptr);
        break;
      case AT_PHNUM:
        wld_printf("AT_PHNUM = %x\n",av->a_un.a_val);
        break;
      case AT_ENTRY:
        wld_printf("AT_ENTRY = %x\n",av->a_un.a_val);
        break;
      case AT_BASE:
        wld_printf("AT_BASE = %x\n",av->a_un.a_val);
        break;
      }
}
#endif

/*
 * set_auxiliary
 *
 * Set a field of the auxiliary structure
 */
static void set_auxiliary( ElfW(auxv_t) *av, int type, long int val )
{
  for ( ; av->a_type != AT_NULL; av++)
    if( av->a_type == type )
    {
      av->a_un.a_val = val;
      return;
    }
  wld_printf( "wine-preloader: cannot set auxiliary value %x, please report\n", type );
}

/*
 * get_auxiliary
 *
 * Get a field of the auxiliary structure
 */
static int get_auxiliary( ElfW(auxv_t) *av, int type, int *val )
{
  for ( ; av->a_type != AT_NULL; av++)
    if( av->a_type == type )
    {
      *val = av->a_un.a_val;
      return 1;
    }
  return 0;
}

/*
 * map_so_lib
 *
 * modelled after _dl_map_object_from_fd() from glibc-2.3.1/elf/dl-load.c
 *
 * This function maps the segments from an ELF object, and optionally
 *  stores information about the mapping into the auxv_t structure.
 */
static void map_so_lib( const char *name, struct wld_link_map *l)
{
    int fd;
    unsigned char buf[0x800];
    ElfW(Ehdr) *header = (ElfW(Ehdr)*)buf;
    ElfW(Phdr) *phdr, *ph;
    /* Scan the program header table, collecting its load commands.  */
    struct loadcmd
      {
        ElfW(Addr) mapstart, mapend, dataend, allocend;
        off_t mapoff;
        int prot;
      } loadcmds[16], *c;
    size_t nloadcmds = 0, maplength;

    fd = open( name, O_RDONLY );
    if (fd == -1) fatal_error("%s: could not open\n", name );

    if (read( fd, buf, sizeof(buf) ) != sizeof(buf))
        fatal_error("%s: failed to read ELF header\n", name);

    phdr = (void*) (((unsigned char*)buf) + header->e_phoff);

    if( ( header->e_ident[0] != 0x7f ) ||
        ( header->e_ident[1] != 'E' ) ||
        ( header->e_ident[2] != 'L' ) ||
        ( header->e_ident[3] != 'F' ) )
        fatal_error( "%s: not an ELF binary... don't know how to load it\n", name );

    if( header->e_machine != EM_386 )
        fatal_error("%s: not an i386 ELF binary... don't know how to load it\n", name );

    if (header->e_phnum > sizeof(loadcmds)/sizeof(loadcmds[0]))
        fatal_error( "%s: oops... not enough space for load commands\n", name );

    maplength = header->e_phnum * sizeof (ElfW(Phdr));
    if (header->e_phoff + maplength > sizeof(buf))
        fatal_error( "%s: oops... not enough space for ELF headers\n", name );

    l->l_ld = 0;
    l->l_addr = 0;
    l->l_phdr = 0;
    l->l_phnum = header->e_phnum;
    l->l_entry = header->e_entry;
    l->l_interp = 0;

    for (ph = phdr; ph < &phdr[l->l_phnum]; ++ph)
    {

#ifdef DUMP_SEGMENTS
      wld_printf( "ph = %x\n", ph );
      wld_printf( " p_type   = %x\n", ph->p_type );
      wld_printf( " p_flags  = %x\n", ph->p_flags );
      wld_printf( " p_offset = %x\n", ph->p_offset );
      wld_printf( " p_vaddr  = %x\n", ph->p_vaddr );
      wld_printf( " p_paddr  = %x\n", ph->p_paddr );
      wld_printf( " p_filesz = %x\n", ph->p_filesz );
      wld_printf( " p_memsz  = %x\n", ph->p_memsz );
      wld_printf( " p_align  = %x\n", ph->p_align );
#endif

      switch (ph->p_type)
        {
          /* These entries tell us where to find things once the file's
             segments are mapped in.  We record the addresses it says
             verbatim, and later correct for the run-time load address.  */
        case PT_DYNAMIC:
          l->l_ld = (void *) ph->p_vaddr;
          l->l_ldnum = ph->p_memsz / sizeof (Elf32_Dyn);
          break;

        case PT_PHDR:
          l->l_phdr = (void *) ph->p_vaddr;
          break;

        case PT_LOAD:
          {
            if ((ph->p_align & page_mask) != 0)
              fatal_error( "%s: ELF load command alignment not page-aligned\n", name );

            if (((ph->p_vaddr - ph->p_offset) & (ph->p_align - 1)) != 0)
              fatal_error( "%s: ELF load command address/offset not properly aligned\n", name );

            c = &loadcmds[nloadcmds++];
            c->mapstart = ph->p_vaddr & ~(ph->p_align - 1);
            c->mapend = ((ph->p_vaddr + ph->p_filesz + page_mask) & ~page_mask);
            c->dataend = ph->p_vaddr + ph->p_filesz;
            c->allocend = ph->p_vaddr + ph->p_memsz;
            c->mapoff = ph->p_offset & ~(ph->p_align - 1);

            c->prot = 0;
            if (ph->p_flags & PF_R)
              c->prot |= PROT_READ;
            if (ph->p_flags & PF_W)
              c->prot |= PROT_WRITE;
            if (ph->p_flags & PF_X)
              c->prot |= PROT_EXEC;
          }
          break;

        case PT_INTERP:
          l->l_interp = ph->p_vaddr;
          break;

        case PT_TLS:
          /*
           * We don't need to set anything up because we're
           * emulating the kernel, not ld-linux.so.2
           * The ELF loader will set up the TLS data itself.
           */
        case PT_SHLIB:
        case PT_NOTE:
        default:
          break;
        }
    }

    /* Now process the load commands and map segments into memory.  */
    c = loadcmds;

    /* Length of the sections to be loaded.  */
    maplength = loadcmds[nloadcmds - 1].allocend - c->mapstart;

    if( header->e_type == ET_DYN )
    {
        ElfW(Addr) mappref;
        mappref = (ELF_PREFERRED_ADDRESS (loader, maplength, c->mapstart)
                   - MAP_BASE_ADDR (l));

        /* Remember which part of the address space this object uses.  */
        l->l_map_start = (ElfW(Addr)) mmap ((void *) mappref, maplength,
                                              c->prot, MAP_COPY | MAP_FILE,
                                              fd, c->mapoff);
        /* wld_printf("set  : offset = %x\n", c->mapoff); */
        /* wld_printf("l->l_map_start = %x\n", l->l_map_start); */

        l->l_map_end = l->l_map_start + maplength;
        l->l_addr = l->l_map_start - c->mapstart;

        mprotect ((caddr_t) (l->l_addr + c->mapend),
                    loadcmds[nloadcmds - 1].allocend - c->mapend,
                    PROT_NONE);
        goto postmap;
    }
    else
    {
        /* sanity check */
        if ((char *)c->mapstart + maplength > preloader_start &&
            (char *)c->mapstart <= preloader_end)
            fatal_error( "%s: binary overlaps preloader (%x-%x)\n",
                         name, c->mapstart, (char *)c->mapstart + maplength );

        ELF_FIXED_ADDRESS (loader, c->mapstart);
    }

    /* Remember which part of the address space this object uses.  */
    l->l_map_start = c->mapstart + l->l_addr;
    l->l_map_end = l->l_map_start + maplength;

    while (c < &loadcmds[nloadcmds])
      {
        if (c->mapend > c->mapstart)
            /* Map the segment contents from the file.  */
            mmap ((void *) (l->l_addr + c->mapstart),
                        c->mapend - c->mapstart, c->prot,
                        MAP_FIXED | MAP_COPY | MAP_FILE, fd, c->mapoff);

      postmap:
        if (l->l_phdr == 0
            && (ElfW(Off)) c->mapoff <= header->e_phoff
            && ((size_t) (c->mapend - c->mapstart + c->mapoff)
                >= header->e_phoff + header->e_phnum * sizeof (ElfW(Phdr))))
          /* Found the program header in this segment.  */
          l->l_phdr = (void *)(unsigned int) (c->mapstart + header->e_phoff - c->mapoff);

        if (c->allocend > c->dataend)
          {
            /* Extra zero pages should appear at the end of this segment,
               after the data mapped from the file.   */
            ElfW(Addr) zero, zeroend, zeropage;

            zero = l->l_addr + c->dataend;
            zeroend = l->l_addr + c->allocend;
            zeropage = (zero + page_mask) & ~page_mask;

            /*
             * This is different from the dl-load load...
             *  ld-linux.so.2 relies on the whole page being zero'ed
             */
            zeroend = (zeroend + page_mask) & ~page_mask;

            if (zeroend < zeropage)
            {
              /* All the extra data is in the last page of the segment.
                 We can just zero it.  */
              zeropage = zeroend;
            }

            if (zeropage > zero)
              {
                /* Zero the final part of the last page of the segment.  */
                if ((c->prot & PROT_WRITE) == 0)
                  {
                    /* Dag nab it.  */
                    mprotect ((caddr_t) (zero & ~page_mask), page_size, c->prot|PROT_WRITE);
                  }
                memset ((void *) zero, '\0', zeropage - zero);
                if ((c->prot & PROT_WRITE) == 0)
                  mprotect ((caddr_t) (zero & ~page_mask), page_size, c->prot);
              }

            if (zeroend > zeropage)
              {
                /* Map the remaining zero pages in from the zero fill FD.  */
                caddr_t mapat;
                mapat = mmap ((caddr_t) zeropage, zeroend - zeropage,
                                c->prot, MAP_ANON|MAP_PRIVATE|MAP_FIXED,
                                -1, 0);
              }
          }

        ++c;
      }

    if (l->l_phdr == NULL) fatal_error("no program header\n");

    l->l_phdr = (void *)((ElfW(Addr))l->l_phdr + l->l_addr);
    l->l_entry += l->l_addr;

    close( fd );
}


/*
 * Find a symbol in the symbol table of the executable loaded
 */
static void *find_symbol( const ElfW(Phdr) *phdr, int num, char *var )
{
    const ElfW(Dyn) *dyn = NULL;
    const ElfW(Phdr) *ph;
    const ElfW(Sym) *symtab = NULL;
    const char *strings = NULL;
    uint32_t i, symtabend = 0;

    /* check the values */
#ifdef DUMP_SYMS
    wld_printf("%x %x\n", phdr, num );
#endif
    if( ( phdr == NULL ) || ( num == 0 ) )
    {
        wld_printf("could not find PT_DYNAMIC header entry\n");
        return NULL;
    }

    /* parse the (already loaded) ELF executable's header */
    for (ph = phdr; ph < &phdr[num]; ++ph)
    {
        if( PT_DYNAMIC == ph->p_type )
        {
            dyn = (void *) ph->p_vaddr;
            num = ph->p_memsz / sizeof (Elf32_Dyn);
            break;
        }
    }
    if( !dyn ) return NULL;

    while( dyn->d_tag )
    {
        if( dyn->d_tag == DT_STRTAB )
            strings = (const char*) dyn->d_un.d_ptr;
        if( dyn->d_tag == DT_SYMTAB )
            symtab = (const ElfW(Sym) *)dyn->d_un.d_ptr;
        if( dyn->d_tag == DT_HASH )
            symtabend = *((const uint32_t *)dyn->d_un.d_ptr + 1);
#ifdef DUMP_SYMS
        wld_printf("%x %x\n", dyn->d_tag, dyn->d_un.d_ptr );
#endif
        dyn++;
    }

    if( (!symtab) || (!strings) ) return NULL;

    for (i = 0; i < symtabend; i++)
    {
        if( ( ELF32_ST_BIND(symtab[i].st_info) == STT_OBJECT ) &&
            ( 0 == strcmp( strings+symtab[i].st_name, var ) ) )
        {
#ifdef DUMP_SYMS
            wld_printf("Found %s -> %x\n", strings+symtab[i].st_name, symtab[i].st_value );
#endif
            return (void*)symtab[i].st_value;
        }
    }
    return NULL;
}

/*
 *  preload_reserve
 *
 * Reserve a range specified in string format
 */
static void preload_reserve( const char *str )
{
    const char *p;
    unsigned long result = 0;
    void *start = NULL, *end = NULL;
    int first = 1;

    for (p = str; *p; p++)
    {
        if (*p >= '0' && *p <= '9') result = result * 16 + *p - '0';
        else if (*p >= 'a' && *p <= 'f') result = result * 16 + *p - 'a' + 10;
        else if (*p >= 'A' && *p <= 'F') result = result * 16 + *p - 'A' + 10;
        else if (*p == '-')
        {
            if (!first) goto error;
            start = (void *)(result & ~page_mask);
            result = 0;
            first = 0;
        }
        else goto error;
    }
    if (!first) end = (void *)((result + page_mask) & ~page_mask);
    else if (result) goto error;  /* single value '0' is allowed */

    /* sanity checks */
    if (end <= start) start = end = NULL;
    else if ((char *)end > preloader_start &&
             (char *)start <= preloader_end)
    {
        wld_printf( "WINEPRELOADRESERVE range %x-%x overlaps preloader %x-%x\n",
                     start, end, preloader_start, preloader_end );
        start = end = NULL;
    }

    /* entry 2 is for the PE exe */
    preload_info[2].addr = start;
    preload_info[2].size = (char *)end - (char *)start;
    return;

error:
    fatal_error( "invalid WINEPRELOADRESERVE value '%s'\n", str );
}


/*
 *  wld_start
 *
 *  Repeat the actions the kernel would do when loading a dynamically linked .so
 *  Load the binary and then its ELF interpreter.
 *  Note, we assume that the binary is a dynamically linked ELF shared object.
 */
void* wld_start( int argc, ... )
{
    int i;
    char **argv, **p;
    char *interp, *reserve = NULL;
    ElfW(auxv_t)* av;
    struct wld_link_map main_binary_map, ld_so_map;
    struct wine_preload_info **wine_main_preload_info;

    argv = (char **)&argc + 1;

    /* skip over the parameters */
    p = argv + argc + 1;

    /* skip over the environment */
    while (*p)
    {
        static const char res[] = "WINEPRELOADRESERVE=";
        if (!strncmp( *p, res, sizeof(res)-1 )) reserve = *p + sizeof(res) - 1;
        p++;
    }

    av = (ElfW(auxv_t)*) (p+1);
    if (!get_auxiliary( av, AT_PAGESZ, &page_size )) page_size = 4096;
    page_mask = page_size - 1;

    preloader_start = (char *)_start - ((unsigned int)_start & page_mask);
    preloader_end = (char *)((unsigned int)(_end + page_mask) & ~page_mask);

#ifdef DUMP_AUX_INFO
    for( i = 0; i<argc; i++ ) wld_printf("argv[%x] = %s\n", i, argv[i]);
    dump_auxiliary( av );
#endif

    /* reserve memory that Wine needs */
    if (reserve) preload_reserve( reserve );
    for (i = 0; preload_info[i].size; i++)
        mmap( preload_info[i].addr, preload_info[i].size,
              PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0 );

    /* load the main binary */
    map_so_lib( argv[0], &main_binary_map );

    /* load the ELF interpreter */
    interp = (char *)main_binary_map.l_addr + main_binary_map.l_interp;
    map_so_lib( interp, &ld_so_map );

    /* store pointer to the preload info into the appropriate main binary variable */
    wine_main_preload_info = find_symbol( main_binary_map.l_phdr, main_binary_map.l_phnum,
                                          "wine_main_preload_info" );
    if (wine_main_preload_info) *wine_main_preload_info = preload_info;
    else wld_printf( "wine_main_preload_info not found\n" );

    set_auxiliary( av, AT_PHDR, (unsigned long)main_binary_map.l_phdr );
    set_auxiliary( av, AT_PHNUM, main_binary_map.l_phnum );
    set_auxiliary( av, AT_BASE, ld_so_map.l_addr );
    set_auxiliary( av, AT_ENTRY, main_binary_map.l_entry );

#ifdef DUMP_AUX_INFO
    wld_printf("New auxiliary info:\n");
    dump_auxiliary( av );
    wld_printf("jumping to %x\n", ld_so_map.l_entry);
#endif

    return (void *)ld_so_map.l_entry;
}
