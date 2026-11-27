/**
 * @file coff_reader.c
 * @brief Windows PE/COFF Relocatable Object (.obj) Reader for ZeroCC (Sprint 259)
 */

#include "zero_cc/linker/coff_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool zero_cc_coff_read_object_mem(const uint8_t *data, size_t size,
                                  const char *name, ZeroCCCoffParsedObject *obj,
                                  char *err, size_t err_size)
{
    if (!data || size < sizeof(ZeroCCCoffFileHeader) || !obj) {
        if (err && err_size > 0) snprintf(err, err_size, "invalid or empty buffer");
        return false;
    }

    memset(obj, 0, sizeof(*obj));
    if (name) {
        strncpy(obj->name, name, sizeof(obj->name) - 1);
        obj->name[sizeof(obj->name) - 1] = '\0';
    }

    const ZeroCCCoffFileHeader *hdr = (const ZeroCCCoffFileHeader *)data;
    if (hdr->machine != ZERO_CC_IMAGE_FILE_MACHINE_AMD64) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "unsupported COFF machine 0x%04x (expected x86-64 0x8664)", hdr->machine);
        }
        return false;
    }

    obj->machine = hdr->machine;
    size_t num_sections = hdr->number_of_sections;
    if (num_sections > ZERO_CC_COFF_MAX_SECTIONS) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "too many sections (%zu > %d)", num_sections, ZERO_CC_COFF_MAX_SECTIONS);
        }
        return false;
    }

    /* Locate symbol table and string table */
    uint32_t symtab_off = hdr->pointer_to_symbol_table;
    uint32_t num_symbols = hdr->number_of_symbols;
    if (symtab_off > 0 && num_symbols > 0) {
        uint64_t symtab_end = (uint64_t)symtab_off + (uint64_t)num_symbols * sizeof(ZeroCCCoffSymbol);
        if (symtab_end + 4 <= size) {
            uint32_t strtab_size = 0;
            memcpy(&strtab_size, data + symtab_end, sizeof(uint32_t));
            if (symtab_end + strtab_size <= size) {
                obj->strtab = (const char *)(data + symtab_end);
                obj->strsize = strtab_size;
            }
        }
    }

    /* Parse sections */
    size_t sec_hdrs_off = sizeof(ZeroCCCoffFileHeader) + hdr->size_of_optional_header;
    if (sec_hdrs_off + num_sections * sizeof(ZeroCCSectionHeader) > size) {
        if (err && err_size > 0) snprintf(err, err_size, "truncated section headers");
        return false;
    }

    for (size_t s = 0; s < num_sections; s++) {
        const ZeroCCSectionHeader *sh = (const ZeroCCSectionHeader *)(data + sec_hdrs_off + s * sizeof(ZeroCCSectionHeader));
        ZeroCCCoffSectParsed *so = &obj->sections[obj->section_count++];

        /* Resolve section name */
        if (sh->name[0] == '/' && obj->strtab) {
            uint32_t soff = (uint32_t)strtoul(sh->name + 1, NULL, 10);
            if (soff < obj->strsize) {
                strncpy(so->name, obj->strtab + soff, sizeof(so->name) - 1);
                so->name[sizeof(so->name) - 1] = '\0';
            } else {
                strncpy(so->name, sh->name, sizeof(so->name) - 1);
                so->name[sizeof(so->name) - 1] = '\0';
            }
        } else {
            char tmp[9];
            memcpy(tmp, sh->name, 8);
            tmp[8] = '\0';
            strncpy(so->name, tmp, sizeof(so->name) - 1);
            so->name[sizeof(so->name) - 1] = '\0';
        }

        so->size_of_raw_data = sh->size_of_raw_data;
        so->pointer_to_raw_data = sh->pointer_to_raw_data;
        so->characteristics = sh->characteristics;

        if (sh->pointer_to_raw_data > 0 && sh->size_of_raw_data > 0) {
            if ((uint64_t)sh->pointer_to_raw_data + sh->size_of_raw_data <= size) {
                so->data = data + sh->pointer_to_raw_data;
            }
        }

        /* Parse relocations */
        if (sh->pointer_to_relocations > 0 && sh->number_of_relocations > 0) {
            uint64_t rel_end = (uint64_t)sh->pointer_to_relocations +
                               (uint64_t)sh->number_of_relocations * sizeof(ZeroCCCoffReloc);
            if (rel_end <= size) {
                so->reloc_count = sh->number_of_relocations;
                so->relocs = (ZeroCCCoffRelocParsed *)calloc(so->reloc_count, sizeof(ZeroCCCoffRelocParsed));
                if (!so->relocs) {
                    if (err && err_size > 0) snprintf(err, err_size, "out of memory allocating relocations");
                    zero_cc_coff_parsed_object_free(obj);
                    return false;
                }
                const ZeroCCCoffReloc *r_raw = (const ZeroCCCoffReloc *)(data + sh->pointer_to_relocations);
                for (size_t r = 0; r < so->reloc_count; r++) {
                    so->relocs[r].virtual_address = r_raw[r].virtual_address;
                    so->relocs[r].symbol_index = r_raw[r].symbol_table_index;
                    so->relocs[r].type = r_raw[r].type;
                }
            }
        }
    }

    /* Parse symbols */
    if (symtab_off > 0 && num_symbols > 0) {
        uint64_t sym_end = (uint64_t)symtab_off + (uint64_t)num_symbols * sizeof(ZeroCCCoffSymbol);
        if (sym_end <= size) {
            const ZeroCCCoffSymbol *sym_raw = (const ZeroCCCoffSymbol *)(data + symtab_off);
            for (uint32_t i = 0; i < num_symbols && i < ZERO_CC_COFF_MAX_SYMBOLS; i++) {
                ZeroCCCoffSymParsed *sp = &obj->symbols[i];
                if (sym_raw[i].name.long_name.zeroes == 0 && sym_raw[i].name.long_name.offset >= 4) {
                    uint32_t soff = sym_raw[i].name.long_name.offset;
                    if (obj->strtab && soff < obj->strsize) {
                        strncpy(sp->name, obj->strtab + soff, sizeof(sp->name) - 1);
                        sp->name[sizeof(sp->name) - 1] = '\0';
                    }
                } else {
                    char tmp[9];
                    memcpy(tmp, sym_raw[i].name.short_name, 8);
                    tmp[8] = '\0';
                    strncpy(sp->name, tmp, sizeof(sp->name) - 1);
                    sp->name[sizeof(sp->name) - 1] = '\0';
                }

                sp->value = sym_raw[i].value;
                sp->section_number = sym_raw[i].section_number;
                sp->type = sym_raw[i].type;
                sp->storage_class = sym_raw[i].storage_class;

                /* Skip auxiliary symbols to preserve raw symbol index alignment */
                uint8_t num_aux = sym_raw[i].number_of_aux_symbols;
                for (uint8_t a = 0; a < num_aux && (i + 1) < num_symbols; a++) {
                    i++;
                }
            }
            obj->symbol_count = num_symbols;
        }
    }

    return true;
}

bool zero_cc_coff_read_object_file(const char *path, ZeroCCCoffParsedObject *obj,
                                   char *err, size_t err_size)
{
    if (!path || !obj) {
        if (err && err_size > 0) snprintf(err, err_size, "invalid arguments");
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err && err_size > 0) snprintf(err, err_size, "failed to open '%s'", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (fsize <= 0) {
        fclose(f);
        if (err && err_size > 0) snprintf(err, err_size, "file '%s' is empty", path);
        return false;
    }

    uint8_t *buffer = (uint8_t *)malloc((size_t)fsize);
    if (!buffer) {
        fclose(f);
        if (err && err_size > 0) snprintf(err, err_size, "out of memory reading '%s'", path);
        return false;
    }

    size_t rd = fread(buffer, 1, (size_t)fsize, f);
    fclose(f);

    if (rd != (size_t)fsize) {
        free(buffer);
        if (err && err_size > 0) snprintf(err, err_size, "short read from '%s'", path);
        return false;
    }

    bool ok = zero_cc_coff_read_object_mem(buffer, (size_t)fsize, path, obj, err, err_size);
    if (ok) {
        obj->file_data = buffer;
        obj->file_size = (size_t)fsize;
    } else {
        free(buffer);
    }
    return ok;
}

void zero_cc_coff_parsed_object_free(ZeroCCCoffParsedObject *obj)
{
    if (!obj) return;
    for (size_t s = 0; s < obj->section_count; s++) {
        if (obj->sections[s].relocs) {
            free(obj->sections[s].relocs);
            obj->sections[s].relocs = NULL;
        }
    }
    if (obj->file_data) {
        free(obj->file_data);
        obj->file_data = NULL;
    }
}

const ZeroCCCoffSectParsed *zero_cc_coff_find_section(const ZeroCCCoffParsedObject *obj,
                                                      const char *name)
{
    if (!obj || !name) return NULL;
    for (size_t i = 0; i < obj->section_count; i++) {
        if (strcmp(obj->sections[i].name, name) == 0) {
            return &obj->sections[i];
        }
    }
    return NULL;
}
