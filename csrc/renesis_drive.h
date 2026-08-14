/* ---------------------------------------------------------------------------
 *  renesis_drive.h -- primary-input drive models (drive.py), C surface
 *  Renesis: energy-aware reversible / adiabatic logic synthesis
 *
 *  v90.6.  The pair (p1, alpha) per primary input, read from a SAIF
 *  activity file exactly as scripts_adiabatic/drive.py reads it:
 *  p1 = T1/(T0+T1) (X/Z time excluded), alpha = TC/cycles, the
 *  stationary lag-one validity bound enforced with Python's verbatim
 *  error.  Downstream consumers take a flat CONDITIONAL TABLE --
 *  (p1, up, dn) per PI in input-list order -- so the mapping and tag
 *  code stays independent of this module.
 *
 *  Author:      Mitchell A. Thornton
 *  Copyright:   (c) 2026 Clearpoint Research, LLC.  All rights reserved.
 *  Created:     Renesis v90.6
 * --------------------------------------------------------------------------- */
#ifndef RENESIS_DRIVE_H
#define RENESIS_DRIVE_H

#include "rsynth.h"

typedef struct RDrive {
    char  **names;               /* nets the file tagged                 */
    double *p1, *alpha;
    int     n;
    double  default_p1;          /* drive.DEFAULT_P1 = 0.5               */
    double  cycles;              /* resolved cycle count (record stamp)  */
    char    source[512];         /* basename for the record stamp        */
} RDrive;

/* drive.from_saif (strict).  cycles < 0 == not given; period < 0 == not
 * given.  On success returns 0; on error returns -1 with Python's
 * ValueError text in err (the driver prints "error: <text>", rc=1, as
 * sys.exit does). */
int rdrive_from_saif(RDrive *d, const char *path, double cycles,
                     double period, char *err, size_t errn);
void rdrive_free(RDrive *d);

/* Drive.pair(net): explicit entry, else (default_p1, indep_alpha). */
void rdrive_pair(const RDrive *d, const char *net, double *p1, double *alpha);

/* The conditional table for a netlist's primary inputs, in input-list
 * order: cond[3k] = p1, cond[3k+1] = up, cond[3k+2] = dn, exactly
 * drive.conditionals (up = alpha/(2*(1-p1)) clamped, dn = alpha/(2*p1)
 * clamped).  Caller frees.  NULL table == uniform (no drive). */
double *rdrive_cond_table(const RDrive *d, const RNet *nl);

#endif /* RENESIS_DRIVE_H */
