/**
 * @file macho_reader.c
 * @brief macOS Mach-O 64-bit Relocatable Object (MH_OBJECT) Reader for ZeroCC (Sprint 255)
 */

#include "zero_cc/linker/macho_reader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool zero_cc_macho_read_object_mem(const uint8_t *data, size_t size,
                                   const char *name, ZeroCCMachoParsedObject *obj,
                                   char *err, size_t err_size)
{
    if (!data || size < sizeof(ZeroCCMachHeader64) || !obj) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "invalid buffer or buffer too small for Mach-O header");
        }
        return false;
    }

    memset(obj, 0, sizeof(*obj));
    if (name) {
        snprintf(obj->name, sizeof(obj->name), "%s", name);
    }

    /* Copy file data buffer */
    obj->file_data = (uint8_t *)malloc(size);
    if (!obj->file_data) {
        if (err && err_size > 0) snprintf(err, err_size, "out of memory allocating file data");
        return false;
    }
    memcpy(obj->file_data, data, size);
    obj->file_size = size;

    /* Validate Header */
    const ZeroCCMachHeader64 *hdr = (const ZeroCCMachHeader64 *)obj->file_data;
    if (hdr->magic != ZERO_CC_MH_MAGIC_64) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "not a 64-bit Mach-O object (magic: 0x%08x, expected 0x%08x)",
                     hdr->magic, ZERO_CC_MH_MAGIC_64);
        }
        zero_cc_macho_parsed_object_free(obj);
        return false;
    }

    if (hdr->filetype != ZERO_CC_MH_OBJECT) {
        if (err && err_size > 0) {
            snprintf(err, err_size, "Mach-O filetype is %u (expected MH_OBJECT=1)", hdr->filetype);
        }
        zero_cc_macho_parsed_object_free(obj);
        return false;
    }

    obj->cputype = hdr->cputype;
    obj->cpusubtype = hdr->cpusubtype;
    obj->filetype = hdr->filetype;

    /* Parse Load Commands */
    size_t cmd_offset = sizeof(ZeroCCMachHeader64);
    for (uint32_t c = 0; c < hdr->ncmds; c++) {
        if (cmd_offset + 8 > size) {
            if (err && err_size > 0) snprintf(err, err_size, "truncated load command header");
            zero_cc_macho_parsed_object_free(obj);
            return false;
        }

        const uint32_t *cmd_hdr = (const uint32_t *)(obj->file_data + cmd_offset);
        uint32_t cmd = cmd_hdr[0];
        uint32_t cmdsize = cmd_hdr[1];

        if (cmdsize < 8 || cmd_offset + cmdsize > size) {
            if (err && err_size > 0) snprintf(err, err_size, "corrupted load command size %u", cmdsize);
            zero_cc_macho_parsed_object_free(obj);
            return false;
        }

        if (cmd == ZERO_CC_LC_SEGMENT_64) {
            if (cmdsize < sizeof(ZeroCCSegmentCommand64)) {
                if (err && err_size > 0) snprintf(err, err_size, "corrupted LC_SEGMENT_64 command");
                zero_cc_macho_parsed_object_free(obj);
                return false;
            }
            const ZeroCCSegmentCommand64 *seg = (const ZeroCCSegmentCommand64 *)(obj->file_data + cmd_offset);
            size_t sect_offset = cmd_offset + sizeof(ZeroCCSegmentCommand64);

            for (uint32_t s = 0; s < seg->nsects; s++) {
                if (sect_offset + sizeof(ZeroCCSection64) > cmd_offset + cmdsize) {
                    break;
                }
                if (obj->section_count >= ZERO_CC_MACHO_MAX_SECTIONS) {
                    break;
                }

                const ZeroCCSection64 *raw_sec = (const ZeroCCSection64 *)(obj->file_data + sect_offset);
                ZeroCCMachoSectParsed *sec = &obj->sections[obj->section_count];

                memcpy(sec->sectname, raw_sec->sectname, 16);
                sec->sectname[15] = '\0';
                memcpy(sec->segname, raw_sec->segname, 16);
                sec->segname[15] = '\0';

                sec->addr = raw_sec->addr;
                sec->size = raw_sec->size;
                sec->offset = raw_sec->offset;
                sec->align = raw_sec->align;
                sec->reloff = raw_sec->reloff;
                sec->nreloc = raw_sec->nreloc;
                sec->flags = raw_sec->flags;

                if (sec->offset > 0 && sec->size > 0 && sec->offset + sec->size <= size) {
                    sec->data = obj->file_data + sec->offset;
                } else {
                    sec->data = NULL;
                }

                /* Parse section relocations */
                if (sec->reloff > 0 && sec->nreloc > 0) {
                    size_t rel_bytes = (size_t)sec->nreloc * sizeof(ZeroCCMachORelocation);
                    if (sec->reloff + rel_bytes <= size) {
                        sec->relocs = (ZeroCCMachoRelocParsed *)calloc(sec->nreloc, sizeof(ZeroCCMachoRelocParsed));
                        if (sec->relocs) {
                            sec->reloc_count = sec->nreloc;
                            for (size_t r = 0; r < sec->nreloc; r++) {
                                const ZeroCCMachORelocation *raw_rel =
                                    (const ZeroCCMachORelocation *)(obj->file_data + sec->reloff +
                                                                    r * sizeof(ZeroCCMachORelocation));
                                sec->relocs[r].address = raw_rel->r_address;
                                uint32_t info = raw_rel->r_info;
                                sec->relocs[r].symbolnum = info & 0x00FFFFFFU;
                                sec->relocs[r].is_pcrel = ((info >> 24) & 1U) != 0;
                                sec->relocs[r].length = (uint8_t)((info >> 25) & 3U);
                                sec->relocs[r].is_extern = ((info >> 27) & 1U) != 0;
                                sec->relocs[r].type = (uint8_t)((info >> 28) & 0x0FU);
                            }
                        }
                    }
                }

                obj->section_count++;
                sect_offset += sizeof(ZeroCCSection64);
            }
        } else if (cmd == ZERO_CC_LC_SYMTAB) {
            if (cmdsize >= sizeof(ZeroCCSymtabCommand)) {
                const ZeroCCSymtabCommand *symcmd = (const ZeroCCSymtabCommand *)(obj->file_data + cmd_offset);
                if (symcmd->stroff + symcmd->strsize <= size) {
                    obj->strtab = (const char *)(obj->file_data + symcmd->stroff);
                    obj->strsize = symcmd->strsize;
                }

                size_t sym_bytes = (size_t)symcmd->nsyms * sizeof(ZeroCCNlist64);
                if (symcmd->symoff + sym_bytes <= size) {
                    uint32_t n = symcmd->nsyms;
                    if (n > ZERO_CC_MACHO_MAX_SYMBOLS) n = ZERO_CC_MACHO_MAX_SYMBOLS;

                    for (uint32_t s = 0; s < n; s++) {
                        const ZeroCCNlist64 *nl =
                            (const ZeroCCNlist64 *)(obj->file_data + symcmd->symoff + s * sizeof(ZeroCCNlist64));
                        ZeroCCMachoSymParsed *sym = &obj->symbols[s];

                        sym->type = nl->n_type;
                        sym->is_extern = (nl->n_type & ZERO_CC_N_EXT) != 0;
                        sym->sect = nl->n_sect;
                        sym->desc = nl->n_desc;
                        sym->value = nl->n_value;

                        if (obj->strtab && nl->n_strx < obj->strsize) {
                            snprintf(sym->name, sizeof(sym->name), "%s", obj->strtab + nl->n_strx);
                        } else {
                            sym->name[0] = '\0';
                        }
                    }
                    obj->symbol_count = n;
                }
            }
        }

        cmd_offset += cmdsize;
    }

    return true;
}

bool zero_cc_macho_read_object_file(const char *path, ZeroCCMachoParsedObject *obj,
                                    char *err, size_t err_size)
{
    if (!path || !obj) {
        if (err && err_size > 0) snprintf(err, err_size, "invalid file path or target pointer");
        return false;
    }

    FILE *f = fopen(path, "rb");
    if (!f) {
        if (err && err_size > 0) snprintf(err, err_size, "failed to open %s", path);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (sz <= 0) {
        fclose(f);
        if (err && err_size > 0) snprintf(err, err_size, "file is empty: %s", path);
        return false;
    }

    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        if (err && err_size > 0) snprintf(err, err_size, "out of memory reading %s", path);
        return false;
    }

    size_t rd = fread(buf, 1, (size_t)sz, f);
    fclose(f);

    if (rd != (size_t)sz) {
        free(buf);
        if (err && err_size > 0) snprintf(err, err_size, "short read on %s", path);
        return false;
    }

    bool ok = zero_cc_macho_read_object_mem(buf, (size_t)sz, path, obj, err, err_size);
    free(buf);
    return ok;
}

void zero_cc_macho_parsed_object_free(ZeroCCMachoParsedObject *obj)
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
    memset(obj, 0, sizeof(*obj));
}

const ZeroCCMachoSectParsed *zero_cc_macho_find_section(const ZeroCCMachoParsedObject *obj,
                                                        const char *segname,
                                                        const char *sectname)
{
    if (!obj || !sectname) return NULL;
    for (size_t s = 0; s < obj->section_count; s++) {
        if (strcmp(obj->sections[s].sectname, sectname) == 0) {
            if (!segname || obj->sections[s].segname[0] == '\0' || strcmp(obj->sections[s].segname, segname) == 0) {
                return &obj->sections[s];
            }
        }
    }
    return NULL;
}
