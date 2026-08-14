/* ---------------------------------------------------------------------------
 *  esc_ident.h -- ============================================================================
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  esc_ident.h -- ONE definition of the escaped-identifier convention,
 *  shared by both C Verilog readers (parse_verilog.c and rsynth_net.c).
 *  v70. IEEE 1364 escaped identifiers (`\name[3] `, terminated by
 *  whitespace) are rewritten to a plain token so that neither reader's
 *  expression grammar has to admit '[' or ']' as identifier characters:
 *  \count[0] -> ESC_count_0_ \dest_x[11] -> ESC_dest_x_11_
 *  This is byte-for-byte the rule in scripts_adiabatic/verilog_front.py
 *  `_normalize_escaped`:
 *  "ESC_" + re.sub(r"[^A-Za-z0-9_]", "_", name) + " "
 *  Chosen over the alternative (strip the backslash, keep `count[0]`)
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Modified:    2026-08-10  (Renesis v89.11)
 *  Created:     Renesis v70 (earliest version token in file)
 * --------------------------------------------------------------------------- */
/* ============================================================================
 * esc_ident.h -- ONE definition of the escaped-identifier convention, shared
 * by both C Verilog readers (parse_verilog.c and rsynth_net.c).
 *
 * v70.  IEEE 1364 escaped identifiers (`\name[3] `, terminated by whitespace)
 * are rewritten to a plain token so that neither reader's expression grammar
 * has to admit '[' or ']' as identifier characters:
 *
 *     \count[0]        ->  ESC_count_0_
 *     \dest_x[11]      ->  ESC_dest_x_11_
 *
 * This is byte-for-byte the rule in scripts_adiabatic/verilog_front.py
 * `_normalize_escaped`:
 *
 *     "ESC_" + re.sub(r"[^A-Za-z0-9_]", "_", name) + " "
 *
 * Chosen over the alternative (strip the backslash, keep `count[0]`) because
 * that form would force the Python tokenizer's identifier class to be widened
 * to accept brackets, loosening the one parser whose strictness is what makes
 * a malformed netlist loud rather than silent.  The flattened form cannot
 * collide with a non-escaped net name, because a non-escaped Verilog
 * identifier cannot contain a bracket.
 *
 * Header-only and static, so both readers compile their own copy from a single
 * source of truth and cannot drift apart.
 * ==========================================================================*/
#ifndef ESC_IDENT_H
#define ESC_IDENT_H

#include <ctype.h>
#include <stddef.h>

/* Worst-case growth: "\a" (2 bytes) -> "ESC_a " (6 bytes).  Callers should
 * size the destination at 3 * srclen + 8. */
#define ESC_GROWTH 3

/* Rewrite every escaped identifier in `src` into `dst`.  Returns the number of
 * identifiers rewritten, so a caller can tell "no escapes present" (0) from
 * "escapes normalised" (>0) -- the 0 case guarantees dst == src byte for byte,
 * which is what makes this a no-op on every file that never had one. */
static int esc_normalize(const char *src, char *dst)
{
    int n = 0;
    size_t o = 0;
    for (size_t i = 0; src[i]; ) {
        if (src[i] != '\\') { dst[o++] = src[i++]; continue; }
        i++;                                   /* consume the backslash */
        dst[o++] = 'E'; dst[o++] = 'S'; dst[o++] = 'C'; dst[o++] = '_';
        while (src[i] && !isspace((unsigned char)src[i])) {
            unsigned char c = (unsigned char)src[i++];
            dst[o++] = (char)((isalnum(c) || c == '_') ? (char)c : '_');
        }
        if (src[i] && isspace((unsigned char)src[i])) i++;  /* the terminator */
        dst[o++] = ' ';                        /* ...re-emitted as a space */
        n++;
    }
    dst[o] = 0;
    return n;
}

#endif /* ESC_IDENT_H */
