/*
 * Copyright (c) 2017 Lima Project
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sub license,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include "util/u_math.h"
#include "util/u_format.h"

#include "lima_context.h"
#include "lima_screen.h"
#include "lima_resource.h"

static void
lima_clear(struct pipe_context *pctx, unsigned buffers,
           const union pipe_color_union *color, double depth, unsigned stencil)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);
   struct lima_context_clear *clear = &ctx->clear;

   clear->buffers = buffers;

   if (buffers & PIPE_CLEAR_COLOR0) {
      clear->color[0] = color->ui[0];
      clear->color[1] = color->ui[1];
      clear->color[2] = color->ui[2];
      clear->color[3] = color->ui[3];
   }

   if (buffers & PIPE_CLEAR_DEPTH)
      clear->depth = fui(depth);

   if (buffers & PIPE_CLEAR_STENCIL)
      clear->stencil = stencil;

   ctx->dirty |= LIMA_CONTEXT_DIRTY_CLEAR;
}

static void
hilbert_rotate(int n, int *x, int *y, int rx, int ry)
{
   if (ry == 0) {
      if (rx == 1) {
         *x = n-1 - *x;
         *y = n-1 - *y;
      }

      /* Swap x and y */
      int t  = *x;
      *x = *y;
      *y = t;
   }
}

static void
hilbert_coords(int n, int d, int *x, int *y)
{
   int rx, ry, i, t=d;

   *x = *y = 0;

   for (i = 0; (1 << i) < n; i++) {

      rx = 1 & (t / 2);
      ry = 1 & (t ^ rx);

      hilbert_rotate(1 << i, x, y, rx, ry);

      *x += rx << i;
      *y += ry << i;

      t /= 4;
   }
}

static void
lima_update_plb(struct lima_context *ctx)
{
   struct lima_context_framebuffer *fb = &ctx->framebuffer;
   struct lima_screen *screen = lima_screen(ctx->base.screen);

   lima_bo_wait(ctx->plb->bo, LIMA_BO_WAIT_FLAG_WRITE, 1000000000, true);

   /* use hilbert_coords to generates 1D to 2D relationship.
    * 1D for pp stream index and 2D for plb block x/y on framebuffer.
    * if multi pp, interleave the 1D index to make each pp's render target
    * close enough which should result close workload
    */
   int max = MAX2(fb->tiled_w, fb->tiled_h);
   int dim = util_logbase2_ceil(max);
   int count = 1 << (dim + dim);
   int index = 0, i;
   int num_pp = screen->info.num_pp;
   uint32_t *stream[4];

   for (i = 0; i < num_pp; i++)
      stream[i] = ctx->plb->map + ctx->plb_pp_offset[i];

   for (i = 0; i < count; i++) {
      int x, y;
      hilbert_coords(max, i, &x, &y);
      if (x < fb->tiled_w && y < fb->tiled_h) {
         int pp = index % num_pp;
         int offset = ((y >> fb->shift_h) * fb->block_w + (x >> fb->shift_w)) * 512;
         int plb_va = ctx->plb->va + offset;

         stream[pp][0] = 0;
         stream[pp][1] = 0xB8000000 | x | (y << 8);
         stream[pp][2] = 0xE0000002 | ((plb_va >> 3) & ~0xE0000003);
         stream[pp][3] = 0xB0000000;

         stream[pp] += 4;
         index++;
      }
   }

   for (i = 0; i < num_pp; i++) {
      stream[i][0] = 0;
      stream[i][1] = 0xBC000000;
   }
}

enum lima_attrib_type {
   LIMA_ATTRIB_FLOAT = 0x000,
   /* todo: find out what lives here. */
   LIMA_ATTRIB_I16   = 0x004,
   LIMA_ATTRIB_U16   = 0x005,
   LIMA_ATTRIB_I8    = 0x006,
   LIMA_ATTRIB_U8    = 0x007,
   LIMA_ATTRIB_I8N   = 0x008,
   LIMA_ATTRIB_U8N   = 0x009,
   LIMA_ATTRIB_I16N  = 0x00A,
   LIMA_ATTRIB_U16N  = 0x00B,
   /* todo: where is the 32 int */
   /* todo: find out what lives here. */
   LIMA_ATTRIB_FIXED = 0x101
};

static enum lima_attrib_type
lima_pipe_format_to_attrib_type(enum pipe_format format)
{
   const struct util_format_description *desc = util_format_description(format);
   int i = util_format_get_first_non_void_channel(format);
   const struct util_format_channel_description *c = desc->channel + i;

   switch (c->type) {
   case UTIL_FORMAT_TYPE_FLOAT:
      return LIMA_ATTRIB_FLOAT;
   case UTIL_FORMAT_TYPE_FIXED:
      return LIMA_ATTRIB_FIXED;
   case UTIL_FORMAT_TYPE_SIGNED:
      if (c->size == 8) {
         if (c->normalized)
            return LIMA_ATTRIB_I8N;
         else
            return LIMA_ATTRIB_I8;
      }
      else if (c->size == 16) {
         if (c->normalized)
            return LIMA_ATTRIB_I16N;
         else
            return LIMA_ATTRIB_I16;
      }
      break;
   case UTIL_FORMAT_TYPE_UNSIGNED:
      if (c->size == 8) {
         if (c->normalized)
            return LIMA_ATTRIB_U8N;
         else
            return LIMA_ATTRIB_U8;
      }
      else if (c->size == 16) {
         if (c->normalized)
            return LIMA_ATTRIB_U16N;
         else
            return LIMA_ATTRIB_U16;
      }
      break;
   }

   return LIMA_ATTRIB_FLOAT;
}

static int
lima_pack_vs_cmd(struct lima_context *ctx, const struct pipe_draw_info *info)
{
   int i = 0;
   uint32_t *vs_cmd = ctx->gp_buffer->map + vs_cmd_offset;

   if (!info->indexed) {
      vs_cmd[i++] = 0x00028000;
      vs_cmd[i++] = 0x50000000;
      vs_cmd[i++] = 0x00000001;
      vs_cmd[i++] = 0x50000000;
   }

   vs_cmd[i++] = ctx->gp_buffer->va + vs_program_offset;
   vs_cmd[i++] = 0x40000000 | ((ctx->vs->shader_size >> 4) << 16);

   /* 3 is prefetch, what's it? */
   vs_cmd[i++] = ((3 - 1) << 20) | ((align(ctx->vs->shader_size, 16) / 16 - 1) << 10);
   vs_cmd[i++] = 0x10000040;

   /* assume to 1 before vs compiler is ready */
   int num_varryings = 1;
   int num_attributes = ctx->vertex_elements->num_elements;

   vs_cmd[i++] = ((num_varryings - 1) << 8) | ((num_attributes - 1) << 24);
   vs_cmd[i++] = 0x10000042;

   vs_cmd[i++] = 0x00000003;
   vs_cmd[i++] = 0x10000041;

   vs_cmd[i++] = ctx->gp_buffer->va + attribute_info_offset;
   vs_cmd[i++] = 0x20000000 | (num_attributes << 17);

   vs_cmd[i++] = ctx->gp_buffer->va + varying_info_offset;
   vs_cmd[i++] = 0x20000008 | (num_varryings << 17);

   vs_cmd[i++] = (info->count << 24) | (info->indexed ? 1 : 0);
   vs_cmd[i++] = 0x00000000 | (info->count >> 8);

   vs_cmd[i++] = 0x00000000;
   vs_cmd[i++] = 0x60000000;

   vs_cmd[i++] = info->indexed ? 0x00018000 : 0x00000000;
   vs_cmd[i++] = 0x50000000;

   return i << 2;
}

static void
lima_draw_vbo(struct pipe_context *pctx, const struct pipe_draw_info *info)
{
   printf("dummy %s\n", __func__);

   struct lima_context *ctx = lima_context(pctx);

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_FRAMEBUFFER) {
      struct lima_context_framebuffer *fb = &ctx->framebuffer;

      if (fb->dirty_dim) {
         lima_update_plb(ctx);
         fb->dirty_dim = false;
      }

      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_FRAMEBUFFER;
   }

   lima_bo_wait(ctx->gp_buffer->bo, LIMA_BO_WAIT_FLAG_WRITE, 1000000000, true);

   if (ctx->dirty & (LIMA_CONTEXT_DIRTY_VERTEX_ELEM|LIMA_CONTEXT_DIRTY_VERTEX_BUFF)) {
      struct lima_vertex_element_state *ve = ctx->vertex_elements;
      struct lima_context_vertex_buffer *vb = &ctx->vertex_buffers;
      uint32_t *attribute = ctx->gp_buffer->map + attribute_info_offset;
      int n = 0;

      for (int i = 0; i < ve->num_elements; i++) {
         struct pipe_vertex_element *pve = ve->pipe + i;

         assert(pve->vertex_buffer_index < vb->count);
         assert(vb->enabled_mask & (1 << pve->vertex_buffer_index));

         struct pipe_vertex_buffer *pvb = vb->vb + pve->vertex_buffer_index;
         struct lima_resource *res = lima_resource(pvb->buffer);
         lima_buffer_update(res->buffer, LIMA_BUFFER_ALLOC_VA);

         /* draw_info start vertex should also be here which is very bad
          * make this bo must be updated also when start vertex change
          * now ignore it first
          */
         attribute[n++] = res->buffer->va + pvb->buffer_offset + pve->src_offset
            //+ info->start * pvb->stride
            ;
         attribute[n++] = (pvb->stride << 11) |
            (lima_pipe_format_to_attrib_type(pve->src_format) << 2) |
            (util_format_get_nr_components(pve->src_format) - 1);

         printf("attribute %d: %x %x\n", i, attribute[n - 2], attribute[n - 1]);
      }

      ctx->dirty &= ~(LIMA_CONTEXT_DIRTY_VERTEX_ELEM|LIMA_CONTEXT_DIRTY_VERTEX_BUFF);
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_SHADER_VERT) {
      struct lima_vs_shader_state *vs = ctx->vs;
      uint32_t *varying = ctx->gp_buffer->map + varying_info_offset;
      int n = 0;

      memcpy(ctx->gp_buffer->map + vs_program_offset, vs->shader, vs->shader_size);

      /* no varing info build for now, just assume only gl_Position
       * it should be built when create vs state with compiled vs info
       */
      varying[n++] = ctx->gp_buffer->va + varying_offset;
      varying[n++] = 0x8020;

      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_SHADER_VERT;
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_SHADER_FRAG) {
      struct lima_fs_shader_state *fs = ctx->fs;

      memcpy(ctx->gp_buffer->map + fs_program_offset, fs->shader, fs->shader_size);
      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_SHADER_FRAG;
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_VIEWPORT) {
      /* should update uniform */
      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_VIEWPORT;
   }

   if (ctx->dirty & LIMA_CONTEXT_DIRTY_SCISSOR) {
      ctx->dirty &= ~LIMA_CONTEXT_DIRTY_SCISSOR;
   }

   int vs_cmd_size = lima_pack_vs_cmd(ctx, info);
   (void)vs_cmd_size;
}

void
lima_draw_init(struct lima_context *ctx)
{
   ctx->base.clear = lima_clear;
   ctx->base.draw_vbo = lima_draw_vbo;
}