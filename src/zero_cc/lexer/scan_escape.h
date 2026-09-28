#ifndef ZERO_CC_LEXER_SCAN_ESCAPE_H
#define ZERO_CC_LEXER_SCAN_ESCAPE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../types_lexer.h"
#include "../lexer.h"


    switch (esc) {
        case 'a': val = '\a'; break;
        case 'b': val = '\b'; break;
        case 'f': val = '\f'; break;
        case 'n': val = '\n'; break;
        case 'r': val = '\r'; break;
        case 't': val = '\t'; break;
        case 'v': val = '\v'; break;
        case '\\': val = '\\'; break;
        case '\'': val = '\''; break;
        case '"': val = '"'; break;
        case '?': val = '?'; break;
        case '0': val = '\0'; break;
        case 'e': case 'E': val = 0x1B; break; /* GNU escape: \e */
        case 'u': {
            /* \uXXXX */
            const char *p = *s + 1;
            int hex = 0, digits = 0;
            while (digits < 4 && isxdigit((unsigned char)*p)) {
                char c = *p++;
                hex = hex * 16 + (isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
                digits++;
            }
            if (digits == 4) {
                val = hex;
                *offset += digits;
                *s = p - 1;
            } else {
                handled = 0;
                val = (unsigned char)'u';
            }
            break;
        }
        case 'U': {
            /* \UXXXXXXXX */
            const char *p = *s + 1;
            int hex = 0, digits = 0;
            while (digits < 8 && isxdigit((unsigned char)*p)) {
                char c = *p++;
                hex = hex * 16 + (isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
                digits++;
            }
            if (digits == 8) {
                val = hex;
                *offset += digits;
                *s = p - 1;
            } else {
                handled = 0;
                val = (unsigned char)'U';
            }
            break;
        }
        case 'x': {
            /* Hex escape: \xhh */
            const char *p = *s + 1;
            int hex = 0;
            int digits = 0;
            while (digits < 2 && isxdigit((unsigned char)*p)) {
                char c = *p++;
                hex = hex * 16 + (isdigit(c) ? c - '0' : (tolower(c) - 'a' + 10));
                digits++;
            }
            if (digits == 0) {
                handled = 0;
                val = (unsigned char)'x';
            } else {
                val = hex;
                *offset += digits;
                *s = p - 1;
            }
            break;
        }
        default:
            /* Octal escape: \ooo (up to 3 octal digits) */
            if (esc >= '0' && esc <= '7') {
                int oct = 0;
                int digits = 0;
                const char *p = *s;
                while (digits < 3 && *p >= '0' && *p <= '7') {
                    oct = oct * 8 + (*p - '0');
                    p++; digits++; (*offset)++;
                }
                val = oct;
                *s = p - 1;
            } else {
                handled = 0;
                val = (unsigned char)esc;
            }
            break;
    }

    if (out_val) *out_val = val;
    if (out_str && handled) *out_str = (char)val;
    return handled;
}

/* Scan a numeric literal (integer or floating point) */

#endif /* ZERO_CC_LEXER_SCAN_ESCAPE_H */
