/* Ranked tier marker and division label. SPDX-License-Identifier: GPL-3.0-or-later */
#include <mu_rank_badge.h>
#include "mu_rank_text.h"
#include <sysdolphin/baselib/gobj.h>
#include <sysdolphin/baselib/gobjproc.h>
#include <sysdolphin/baselib/memory.h>
#include <sysdolphin/baselib/sislib.h>
#include <dolphin/gx.h>
#include <math.h>
#include <string.h>


struct MuRankBadge {
    HSD_Text* text;
    struct MuRankBadge* next;
    void (*text_destroy)(void*);
    MuPlayerRank rank;
    float x, y, size, offset, scale, alpha, glow;
    unsigned age;
    int subtext, art_only;
};
static MuRankBadge* badges;

/* Native v1 has no OS accessibility preference in its host ABI. This explicit
 * presentation-only override applies to the reveal AND all Challenger badges.
 * It reads this one named flag; never enumerate the process environment. */
char* getenv(const char* name);
int mu_rank_badge_reduced_motion(void)
{
    const char* flag = getenv("MU_REDUCED_MOTION");
    return flag && (strcmp(flag, "1") == 0 || strcmp(flag, "true") == 0);
}

static MuRankBadge* badge_for_text(void* text)
{
    MuRankBadge* b;
    for (b = badges; b; b = b->next) if (b->text == text) return b;
    return NULL;
}

static void badge_destroy(void* data)
{
    MuRankBadge** link = &badges;
    while (*link) {
        MuRankBadge* b = *link;
        if (b->text == data) {
            void (*destroy)(void*) = b->text_destroy;
            *link = b->next;
            HSD_Free(b);
            destroy(data);
            return;
        }
        link = &b->next;
    }
}

static void badge_draw(void* object)
{
    HSD_GObj* gobj = object;
    MuRankBadge* b = badge_for_text(gobj->user_data);
    if (!b || !b->rank.placed || b->alpha <= 0) return;
    unsigned rgb = mu_rank_text_rgb(b->rank.tier);
    GXColor tint = { rgb >> 16, rgb >> 8, rgb, (u8)(255 * b->alpha) };
    float radius = 8 * b->size * b->scale;
    float x = b->x + 12 * b->size, y = -(b->y + b->offset + 12 * b->size);
    GXSetTevColor(GX_TEVREG0, tint);
    GXSetTevColorIn(GX_TEVSTAGE0, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_C0);
    GXSetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_A0);
    GXBegin(GX_QUADS, GX_VTXFMT0, 4);
    GXPosition3f32(x, y + radius, b->text->pos_z); GXTexCoord2f32(0,0);
    GXPosition3f32(x + radius, y, b->text->pos_z); GXTexCoord2f32(1,0);
    GXPosition3f32(x, y - radius, b->text->pos_z); GXTexCoord2f32(1,1);
    GXPosition3f32(x - radius, y, b->text->pos_z); GXTexCoord2f32(0,1);
    GXEnd();
}

MuRankBadge* mu_rank_badge_create(struct HSD_GObj* parent, float x, float y, float scale, const MuPlayerRank* rank)
{
    MuRankBadge* b;
    /* SIS owns the drawing entity independently, as in the shared fallback.
     * Callers may free before their parent; scene teardown also frees it. */
    (void)parent;
    if (scale <= 0) return NULL;
    b = HSD_MemAlloc(sizeof(*b));
    if (!b) return NULL;
    memset(b, 0, sizeof *b);
    b->text = mu_rank_text_create();
    if (!b->text) { HSD_Free(b); return NULL; }
    b->text->pos_x = x; b->text->pos_y = y;
    b->text->box_size_x = b->text->box_size_y = 20;
    b->x = x; b->y = y; b->size = scale; b->scale = b->alpha = 1;
    b->text->x34.x = b->text->x34.y = .35f*scale;
    b->text->default_kerning = 1;
    /* The division numeral sits centered under the art (only the setup screen draws it; CSS and
     * reveal are art only). At 80 units to the right it fell off the 640-wide setup screen for
     * the right-hand badge and floated under the left player's name (build 31, 11-01).
     * Build 37: at .35*scale it crossed the setup header's bottom border (glyph about y 180-207
     * px, border about 195), so it is a size smaller and sits between the art and that border.
     * SIS draws a glyph about 35*(its scale) below its row (measured on build 31's frames). */
    b->text->default_alignment = 1;
    b->subtext = HSD_SisLib_803A6B98(b->text, 12*scale, 11.4f*scale, "%s", "");
    HSD_SisLib_803A7548(b->text, b->subtext, .25f*scale, .25f*scale);
    b->text_destroy = b->text->entity->user_data_remove_func;
    b->text->entity->user_data_remove_func = badge_destroy;
    b->text->render_callback = badge_draw;
    b->next = badges; badges = b;
    mu_rank_badge_set(b, rank);
    return b;
}

void mu_rank_badge_set(MuRankBadge* b, const MuPlayerRank* rank)
{
    static const char* const roman[] = {"", "I", "II", "III", "IV"};
    const char* numeral = "";
    if (!b) return;
    if (rank) b->rank = *rank;
    else memset(&b->rank, 0, sizeof b->rank);
    if (!b->rank.known) numeral = "?";
    else if (b->rank.placed && b->rank.tier >= 1 && b->rank.tier <= 6 && b->rank.division <= 3)
        numeral = roman[b->rank.division];
    HSD_SisLib_803A70A0(b->text, b->subtext, "%s", b->art_only ? "" : numeral);
}

/* CSS has a separate full label; keep enlarged art within its column rather
 * than repeating the division and Practice word on the badge's right edge. */
void mu_rank_badge_art_only(MuRankBadge* b)
{
    if (!b) return;
    b->art_only = 1;
    HSD_SisLib_803A70A0(b->text, b->subtext, "%s", "");
}

/* Private to Track C: offsets in SIS world units; other callers retain the
 * shared create/set/free interface. Alpha/scale affect the division too. */
void mu_rank_badge_pose(MuRankBadge* b, float y, float scale, float alpha, float glow)
{
    GXColor white = {255,255,255,255};
    if (!b) return;
    b->offset = y; b->scale = scale; b->alpha = alpha; b->glow = glow;
    b->text->pos_y = b->y+y;
    b->text->x34.x = b->text->x34.y = .35f*b->size*scale;
    white.a = (u8)(alpha*255);
    HSD_SisLib_803A74F0(b->text, b->subtext, &white);
}

void mu_rank_badge_free(MuRankBadge* b)
{
    if (b) HSD_SisLib_803A5CC4(b->text);
}
