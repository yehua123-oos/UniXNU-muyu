/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#ifndef SHENG_GMU_VOTES_H
#define SHENG_GMU_VOTES_H
#include "AdrenoHfiVotes.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {const uint8_t *data;size_t bytes;} ShengGmuAux;
typedef struct {uint32_t address;ShengGmuAux aux;} ShengGmuBcmInput;
typedef struct {
    uint32_t chip_id;
    ShengGmuAux gfx,mx,cx,gmxc;
    unsigned gmxc_present; /* 0=authoritatively absent, 1=present; unknown must refuse */
    ShengGmuBcmInput sh0,mc0,acv;
    uint32_t cn0_address;
    unsigned enable_acd,enable_ifpc;
} ShengGmuVoteInputs;
/* Build only CPU-side configuration from pinned board OPP/catalog policy and
 * actual cmd-db data supplied by the platform. No device read/write occurs.
 * Missing GMxC data must not silently become absent: discovery errors defer
 * this call. Caller authenticates chip/board/rail/resource ownership and must
 * establish AOSS ACD/IFPC prerequisites before ColdStart, if requested here.
 * Target pinned DT has no OPP speed-bin masks; every enabled OPP is retained.
 * All input/aux/output storage is disjoint and immutable during the call.
 * Failure leaves output unchanged. Software fixtures are not board evidence. */
AdrenoHfiStartupStatus ShengGmuBuildStartup(const ShengGmuVoteInputs *,AdrenoHfiStartupConfig *);
/* Same build with observations: why = bit per refused check (1 arguments/chip id, 2 GPU ARC votes, 4 GMU ARC votes,
 * 8 BCM aux, 16 bandwidth, 32 validation); detail = failing BCM index; gpu/gmu_match count the table levels found in
 * the board's own rail data and first_missing is the first table level absent from it; *_bytes are the cmd-db rail
 * spans. Status, policy and side effects are identical to ShengGmuBuildStartup. Read-only classification only. */
typedef struct {
    uint32_t why,detail,ok;
    uint32_t chip_id,gmxc_present;
    uint32_t gfx_bytes,cx_bytes,mx_bytes,gmxc_bytes;
    uint32_t gpu_match,gpu_total,gpu_first_missing;
    uint32_t gmu_match,gmu_total,gmu_first_missing;
} ShengGmuBuildDiag;
AdrenoHfiStartupStatus ShengGmuBuildStartupDiag(const ShengGmuVoteInputs *,AdrenoHfiStartupConfig *,ShengGmuBuildDiag *);
/* Chip id pinned by the generated ShengGmuVotesSource.h; callers must use it rather than a literal. */
uint32_t ShengGmuVotesChipId(void);
#ifdef __cplusplus
}
#endif
#endif
