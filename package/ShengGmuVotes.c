/* SPDX-License-Identifier: BSD-2-Clause-Patent */
#include "ShengGmuVotes.h"
#include "ShengGmuVotesSource.h"
/* Why bit per refusal, so one boot can say which check stopped the build instead of only that the result was
 * AdrenoHfiStartupInvalid (every failure returns that same status). The policy values stay in the generated header:
 * this only classifies and counts, it never substitutes a guess for a failed check. */
enum {kWhyArgs = 1u << 0, kWhyGpuVotes = 1u << 1, kWhyGmuVotes = 1u << 2, kWhyBcm = 1u << 3,
    kWhyBandwidth = 1u << 4, kWhyValidate = 1u << 5};
/* The chip id the policy was generated for. Callers must ask instead of repeating the constant: the probe used to
 * pass the A740 id (0x43050a01) while this header pins the board's own Adreno735 id (0x43030b00), and the first
 * check below rejected every call before a single vote was built. */
uint32_t ShengGmuVotesChipId(void)
{
    return SHENG_GMU_VOTES_CHIP_ID;
}
/* AdrenoHfiArcVotes walks the primary rail looking for each table level; a level above the rail's own maximum makes
 * it refuse the whole vote. Counting the matches says whether the generated table and this board's command-db agree,
 * which is the one thing the status alone cannot distinguish from a malformed rail. */
static void arc_match(const ShengGmuOpp *opps,unsigned count,const ShengGmuAux *rail,ShengGmuBuildDiag *d,int gmu)
{
    uint32_t *match = gmu ? &d->gmu_match : &d->gpu_match;
    uint32_t *total = gmu ? &d->gmu_total : &d->gpu_total;
    uint32_t *missing = gmu ? &d->gmu_first_missing : &d->gpu_first_missing;
    *total = count;
    for(unsigned i = 0; i < count; ++i) {
        const uint16_t want = (uint16_t)opps[i].level;
        int found = 0;
        for(size_t at = 0; rail->data && at + 1 < rail->bytes; at += 2)
            if((uint16_t)(rail->data[at] | rail->data[at + 1] << 8) == want) {found = 1; break;}
        if(found) ++*match;
        else if(!*missing) *missing = want;
    }
}
AdrenoHfiStartupStatus ShengGmuBuildStartupDiag(const ShengGmuVoteInputs *in,AdrenoHfiStartupConfig *out,
    ShengGmuBuildDiag *d)
{
    ShengGmuBuildDiag stage = {0};
    stage.why = kWhyArgs;
    stage.gfx_bytes = in ? (uint32_t)in->gfx.bytes : 0;
    stage.cx_bytes = in ? (uint32_t)in->cx.bytes : 0;
    stage.mx_bytes = in ? (uint32_t)in->mx.bytes : 0;
    stage.gmxc_bytes = in ? (uint32_t)in->gmxc.bytes : 0;
    stage.gmxc_present = in ? in->gmxc_present : 0;
    stage.chip_id = in ? in->chip_id : 0;
    if(d) *d = stage;
    if(!in || !out || in->chip_id != SHENG_GMU_VOTES_CHIP_ID || in->gmxc_present > 1 ||
        in->enable_acd > 1 || in->enable_ifpc > 1 ||
        (!in->gmxc_present && (in->gmxc.data || in->gmxc.bytes))) return AdrenoHfiStartupInvalid;
    arc_match(sheng_gpu_opps, SHENG_GPU_OPPS, &in->gfx, &stage, 0);
    arc_match(sheng_gmu_opps, SHENG_GMU_OPPS, &in->cx, &stage, 1);
    stage.why = 0;
    if(d) *d = stage;
    AdrenoHfiStartupConfig c = {0};
    uint32_t votes[16], levels[16];
    const ShengGmuAux *gpu_secondary = in->gmxc_present ? &in->gmxc : &in->mx;
    for(unsigned i = 0; i < SHENG_GPU_OPPS; ++i) levels[i] = sheng_gpu_opps[i].level;
    if(AdrenoHfiArcVotes(levels, SHENG_GPU_OPPS, in->gfx.data, in->gfx.bytes,
        gpu_secondary->data, gpu_secondary->bytes, votes, 16)) {
        stage.why = kWhyGpuVotes; if(d) *d = stage; return AdrenoHfiStartupInvalid;
    }
    c.gpu_count = SHENG_GPU_OPPS;
    for(unsigned i = 0; i < c.gpu_count; ++i) {
        c.gpu[i] = (AdrenoHfiPerformanceLevel){sheng_gpu_opps[i].hz, votes[i]};
        if(in->enable_acd && sheng_gpu_opps[i].acd_present) {
            c.acd_enable_mask |= 1u << i; c.acd_data[c.acd_count++] = sheng_gpu_opps[i].acd;
        }
    }
    if(c.acd_count) c.acd_version = c.acd_stride = 1;
    for(unsigned i = 0; i < SHENG_GMU_OPPS; ++i) levels[i] = sheng_gmu_opps[i].level;
    if(AdrenoHfiArcVotes(levels, SHENG_GMU_OPPS, in->cx.data, in->cx.bytes,
        in->mx.data, in->mx.bytes, votes, 16)) {
        stage.why = kWhyGmuVotes; if(d) *d = stage; return AdrenoHfiStartupInvalid;
    }
    c.gmu_count = SHENG_GMU_OPPS;
    for(unsigned i = 0; i < c.gmu_count; ++i) c.gmu[i] = (AdrenoHfiPerformanceLevel){sheng_gmu_opps[i].hz, votes[i]};
    const ShengGmuBcmInput *inputs[] = {&in->sh0, &in->mc0, &in->acv};
    AdrenoHfiBcm bcms[3];
    for(unsigned i = 0; i < 3; ++i) {
        bcms[i] = sheng_bcm_policy[i]; bcms[i].address = inputs[i]->address;
        if(AdrenoHfiBcmAux(bcms + i, inputs[i]->aux.data, inputs[i]->aux.bytes)) {
            stage.why = kWhyBcm; stage.detail = i; if(d) *d = stage; return AdrenoHfiStartupInvalid;
        }
    }
    if(AdrenoHfiBuildBandwidth(sheng_gpu_bandwidths, SHENG_GPU_BANDWIDTHS, bcms, 3, in->cn0_address, &c)) {
        stage.why = kWhyBandwidth; if(d) *d = stage; return AdrenoHfiStartupInvalid;
    }
    c.enable_ifpc = in->enable_ifpc;
    if(AdrenoHfiStartupValidate(&c)) {
        stage.why = kWhyValidate; if(d) *d = stage; return AdrenoHfiStartupInvalid;
    }
    stage.ok = 1;
    if(d) *d = stage;
    *out = c; return AdrenoHfiStartupSuccess;
}
AdrenoHfiStartupStatus ShengGmuBuildStartup(const ShengGmuVoteInputs *in,AdrenoHfiStartupConfig *out)
{
    return ShengGmuBuildStartupDiag(in,out,0);
}
