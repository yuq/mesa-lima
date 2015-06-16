/*
 * Mesa 3-D graphics library
 *
 * Copyright (C) 2012-2015 LunarG, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors:
 *    Chia-I Wu <olv@lunarg.com>
 */

#include "ilo_debug.h"
#include "ilo_state_vf.h"

static bool
vf_validate_gen6_elements(const struct ilo_dev *dev,
                          const struct ilo_state_vf_info *info)
{
   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 95:
    *
    *     "(Source Element Offset (in bytes))
    *      Format: U11
    *      Range [0,2047"
    *
    * From the Haswell PRM, volume 2d, page 415:
    *
    *     "(Source Element Offset)
    *      Format: U12 byte offset
    *      ...
    *      [0,4095]"
    *
    * From the Broadwell PRM, volume 2d, page 469:
    *
    *     "(Source Element Offset)
    *      Format: U12 byte offset
    *      ...
    *      [0,2047]"
    */
   const uint16_t max_vertex_offset =
      (ilo_dev_gen(dev) == ILO_GEN(7.5)) ? 4096 : 2048;
   uint8_t i;

   ILO_DEV_ASSERT(dev, 6, 8);

   assert(info->element_count <= ILO_STATE_VF_MAX_ELEMENT_COUNT);

   for (i = 0; i < info->element_count; i++) {
      const struct ilo_state_vf_element_info *elem = &info->elements[i];

      assert(elem->buffer < ILO_STATE_VF_MAX_BUFFER_COUNT);
      assert(elem->vertex_offset < max_vertex_offset);

      /*
       * From the Sandy Bridge PRM, volume 2 part 1, page 86:
       *
       *     "64-bit floating point values must be 64-bit aligned in memory,
       *      or UNPREDICTABLE data will be fetched. When accessing an element
       *      containing 64-bit floating point values, the Buffer Starting
       *      Address and Source Element Offset values must add to a 64-bit
       *      aligned address, and BufferPitch must be a multiple of 64-bits."
       */
      if (elem->is_double)
         assert(elem->vertex_offset % 8 == 0);
   }

   return true;
}

static uint32_t
get_gen6_component_controls(const struct ilo_dev *dev,
                            enum gen_vf_component comp_x,
                            enum gen_vf_component comp_y,
                            enum gen_vf_component comp_z,
                            enum gen_vf_component comp_w)
{
   ILO_DEV_ASSERT(dev, 6, 8);

   return comp_x << GEN6_VE_DW1_COMP0__SHIFT |
          comp_y << GEN6_VE_DW1_COMP1__SHIFT |
          comp_z << GEN6_VE_DW1_COMP2__SHIFT |
          comp_w << GEN6_VE_DW1_COMP3__SHIFT;
}

static bool
get_gen6_edge_flag_format(const struct ilo_dev *dev,
                          const struct ilo_state_vf_element_info *elem,
                          enum gen_surface_format *format)
{
   ILO_DEV_ASSERT(dev, 6, 8);

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 94:
    *
    *     "The Source Element Format must be set to the UINT format."
    *
    * From the Haswell PRM, volume 2d, page 413:
    *
    *     "The SourceElementFormat needs to be a single-component format with
    *      an element which has edge flag enabled."
    */
   if (elem->component_count != 1)
      return false;

   /* pick the format we like */
   switch (elem->format_size) {
   case 1:
      *format = GEN6_FORMAT_R8_UINT;
      break;
   case 2:
      *format = GEN6_FORMAT_R16_UINT;
      break;
   case 4:
      *format = GEN6_FORMAT_R32_UINT;
      break;
   default:
      return false;
      break;
   }

   return true;
}

static bool
vf_set_gen6_3DSTATE_VERTEX_ELEMENTS(struct ilo_state_vf *vf,
                                    const struct ilo_dev *dev,
                                    const struct ilo_state_vf_info *info)
{
   enum gen_surface_format edge_flag_format;
   uint32_t dw0, dw1;
   uint8_t i;

   ILO_DEV_ASSERT(dev, 6, 8);

   if (!vf_validate_gen6_elements(dev, info))
      return false;

   for (i = 0; i < info->element_count; i++) {
      const struct ilo_state_vf_element_info *elem = &info->elements[i];
      enum gen_vf_component components[4] = {
         GEN6_VFCOMP_STORE_0,
         GEN6_VFCOMP_STORE_0,
         GEN6_VFCOMP_STORE_0,
         (elem->is_integer) ? GEN6_VFCOMP_STORE_1_INT :
                              GEN6_VFCOMP_STORE_1_FP,
      };

      switch (elem->component_count) {
      case 4: components[3] = GEN6_VFCOMP_STORE_SRC; /* fall through */
      case 3: components[2] = GEN6_VFCOMP_STORE_SRC; /* fall through */
      case 2: components[1] = GEN6_VFCOMP_STORE_SRC; /* fall through */
      case 1: components[0] = GEN6_VFCOMP_STORE_SRC; break;
      default:
              assert(!"unexpected component count");
              break;
      }

      dw0 = elem->buffer << GEN6_VE_DW0_VB_INDEX__SHIFT |
            GEN6_VE_DW0_VALID |
            elem->format << GEN6_VE_DW0_FORMAT__SHIFT |
            elem->vertex_offset << GEN6_VE_DW0_VB_OFFSET__SHIFT;
      dw1 = get_gen6_component_controls(dev,
            components[0], components[1],
            components[2], components[3]);

      STATIC_ASSERT(ARRAY_SIZE(vf->user_ve[i]) >= 2);
      vf->user_ve[i][0] = dw0;
      vf->user_ve[i][1] = dw1;
   }

   vf->user_ve_count = i;

   vf->edge_flag_supported = (i && get_gen6_edge_flag_format(dev,
         &info->elements[i - 1], &edge_flag_format));
   if (vf->edge_flag_supported) {
      const struct ilo_state_vf_element_info *elem = &info->elements[i - 1];

      /* without edge flag enable */
      vf->last_user_ve[0][0] = dw0;
      vf->last_user_ve[0][1] = dw1;

      /*
       * From the Sandy Bridge PRM, volume 2 part 1, page 94:
       *
       *     "This bit (Edge Flag Enable) must only be ENABLED on the last
       *      valid VERTEX_ELEMENT structure.
       *
       *      When set, Component 0 Control must be set to
       *      VFCOMP_STORE_SRC, and Component 1-3 Control must be set to
       *      VFCOMP_NOSTORE."
       */
      dw0 = elem->buffer << GEN6_VE_DW0_VB_INDEX__SHIFT |
            GEN6_VE_DW0_VALID |
            edge_flag_format << GEN6_VE_DW0_FORMAT__SHIFT |
            GEN6_VE_DW0_EDGE_FLAG_ENABLE |
            elem->vertex_offset << GEN6_VE_DW0_VB_OFFSET__SHIFT;
      dw1 = get_gen6_component_controls(dev, GEN6_VFCOMP_STORE_SRC,
            GEN6_VFCOMP_NOSTORE, GEN6_VFCOMP_NOSTORE, GEN6_VFCOMP_NOSTORE);

      /* with edge flag enable */
      vf->last_user_ve[1][0] = dw0;
      vf->last_user_ve[1][1] = dw1;
   }

   return true;
}

static uint32_t
get_gen6_component_zeros(const struct ilo_dev *dev)
{
   ILO_DEV_ASSERT(dev, 6, 8);

   return get_gen6_component_controls(dev,
         GEN6_VFCOMP_STORE_0,
         GEN6_VFCOMP_STORE_0,
         GEN6_VFCOMP_STORE_0,
         GEN6_VFCOMP_STORE_0);
}

static uint32_t
get_gen6_component_ids(const struct ilo_dev *dev,
                       bool vertexid, bool instanceid)
{
   ILO_DEV_ASSERT(dev, 6, 7.5);

   return get_gen6_component_controls(dev,
      (vertexid) ? GEN6_VFCOMP_STORE_VID : GEN6_VFCOMP_STORE_0,
      (instanceid) ? GEN6_VFCOMP_STORE_IID : GEN6_VFCOMP_STORE_0,
      GEN6_VFCOMP_STORE_0,
      GEN6_VFCOMP_STORE_0);
}

static bool
vf_params_set_gen6_internal_ve(struct ilo_state_vf *vf,
                               const struct ilo_dev *dev,
                               const struct ilo_state_vf_params_info *params,
                               uint8_t user_ve_count)
{
   const bool prepend_ids =
      (params->prepend_vertexid || params->prepend_instanceid);
   uint8_t internal_ve_count = 0;

   ILO_DEV_ASSERT(dev, 6, 8);

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 92:
    *
    *     "- At least one VERTEX_ELEMENT_STATE structure must be included.
    *
    *      - Inclusion of partial VERTEX_ELEMENT_STATE structures is
    *        UNDEFINED.
    *
    *      - SW must ensure that at least one vertex element is defined prior
    *        to issuing a 3DPRIMTIVE command, or operation is UNDEFINED.
    *
    *      - There are no "holes" allowed in the destination vertex: NOSTORE
    *        components must be overwritten by subsequent components unless
    *        they are the trailing DWords of the vertex.  Software must
    *        explicitly chose some value (probably 0) to be written into
    *        DWords that would otherwise be "holes"."
    *
    *      - ...
    *
    *      - [DevILK+] Element[0] must be valid."
    */
   if (params->prepend_zeros || (!user_ve_count && !prepend_ids)) {
      STATIC_ASSERT(ARRAY_SIZE(vf->internal_ve[internal_ve_count]) >= 2);
      vf->internal_ve[internal_ve_count][0] = GEN6_VE_DW0_VALID;
      vf->internal_ve[internal_ve_count][1] = get_gen6_component_zeros(dev);
      internal_ve_count++;
   }

   if (prepend_ids) {
      uint32_t dw1;

      if (ilo_dev_gen(dev) >= ILO_GEN(8)) {
         /* placeholder for 3DSTATE_VF_SGVS */
         dw1 = get_gen6_component_zeros(dev);
      } else {
         dw1 = get_gen6_component_ids(dev,
               params->prepend_vertexid,
               params->prepend_instanceid);
      }

      STATIC_ASSERT(ARRAY_SIZE(vf->internal_ve[internal_ve_count]) >= 2);
      vf->internal_ve[internal_ve_count][0] = GEN6_VE_DW0_VALID;
      vf->internal_ve[internal_ve_count][1] = dw1;
      internal_ve_count++;
   }

   vf->internal_ve_count = internal_ve_count;

   return true;
}

static bool
vf_params_set_gen8_3DSTATE_VF_SGVS(struct ilo_state_vf *vf,
                                   const struct ilo_dev *dev,
                                   const struct ilo_state_vf_params_info *params)
{
   const uint8_t attr = (params->prepend_zeros) ? 1 : 0;
   uint32_t dw1;

   ILO_DEV_ASSERT(dev, 8, 8);

   dw1 = 0;

   if (params->prepend_instanceid) {
      dw1 |= GEN8_SGVS_DW1_IID_ENABLE |
             1 << GEN8_SGVS_DW1_IID_VE_COMP__SHIFT |
             attr << GEN8_SGVS_DW1_IID_VE_INDEX__SHIFT;
   }

   if (params->prepend_vertexid) {
      dw1 |= GEN8_SGVS_DW1_VID_ENABLE |
             0 << GEN8_SGVS_DW1_VID_VE_COMP__SHIFT |
             attr << GEN8_SGVS_DW1_VID_VE_INDEX__SHIFT;
   }

   STATIC_ASSERT(ARRAY_SIZE(vf->sgvs) >= 1);
   vf->sgvs[0] = dw1;

   return true;
}

static uint32_t
get_gen6_fixed_cut_index(const struct ilo_dev *dev,
                         enum gen_index_format format)
{
   const uint32_t fixed = ~0u;

   ILO_DEV_ASSERT(dev, 6, 7);

   switch (format) {
   case GEN6_INDEX_BYTE:   return (uint8_t)  fixed;
   case GEN6_INDEX_WORD:   return (uint16_t) fixed;
   case GEN6_INDEX_DWORD:  return (uint32_t) fixed;
   default:
      assert(!"unknown index format");
      return fixed;
   }
}

static bool
get_gen6_cut_index_supported(const struct ilo_dev *dev,
                             enum gen_3dprim_type topology)
{
   ILO_DEV_ASSERT(dev, 6, 8);

   /*
    * See the Sandy Bridge PRM, volume 2 part 1, page 80 and the Haswell PRM,
    * volume 7, page 456.
    */
   switch (topology) {
   case GEN6_3DPRIM_TRIFAN:
   case GEN6_3DPRIM_QUADLIST:
   case GEN6_3DPRIM_QUADSTRIP:
   case GEN6_3DPRIM_POLYGON:
   case GEN6_3DPRIM_LINELOOP:
      return (ilo_dev_gen(dev) >= ILO_GEN(7.5));
   case GEN6_3DPRIM_RECTLIST:
   case GEN6_3DPRIM_TRIFAN_NOSTIPPLE:
      return false;
   default:
      return true;
   }
}

static bool
vf_params_set_gen6_3dstate_index_buffer(struct ilo_state_vf *vf,
                                        const struct ilo_dev *dev,
                                        const struct ilo_state_vf_params_info *params)
{
   uint32_t dw0 = 0;

   ILO_DEV_ASSERT(dev, 6, 7);

   /* cut index only, as in 3DSTATE_VF */
   if (params->cut_index_enable) {
      assert(get_gen6_cut_index_supported(dev, params->cv_topology));
      assert(get_gen6_fixed_cut_index(dev, params->cv_index_format) ==
            params->cut_index);

      dw0 |= GEN6_IB_DW0_CUT_INDEX_ENABLE;
   }

   STATIC_ASSERT(ARRAY_SIZE(vf->cut) >= 1);
   vf->cut[0] = dw0;

   return true;
}

static bool
vf_params_set_gen75_3DSTATE_VF(struct ilo_state_vf *vf,
                               const struct ilo_dev *dev,
                               const struct ilo_state_vf_params_info *params)
{
   uint32_t dw0 = 0;

   ILO_DEV_ASSERT(dev, 7.5, 8);

   if (params->cut_index_enable) {
      assert(get_gen6_cut_index_supported(dev, params->cv_topology));
      dw0 |= GEN75_VF_DW0_CUT_INDEX_ENABLE;
   }

   STATIC_ASSERT(ARRAY_SIZE(vf->cut) >= 2);
   vf->cut[0] = dw0;
   vf->cut[1] = params->cut_index;

   return true;
}

bool
ilo_state_vf_init(struct ilo_state_vf *vf,
                  const struct ilo_dev *dev,
                  const struct ilo_state_vf_info *info)
{
   bool ret = true;

   assert(ilo_is_zeroed(vf, sizeof(*vf)));
   assert(ilo_is_zeroed(info->data, info->data_size));

   assert(ilo_state_vf_data_size(dev, info->element_count) <=
         info->data_size);
   vf->user_ve = (uint32_t (*)[2]) info->data;

   ret &= vf_set_gen6_3DSTATE_VERTEX_ELEMENTS(vf, dev, info);

   ret &= ilo_state_vf_set_params(vf, dev, &info->params);

   assert(ret);

   return ret;
}

bool
ilo_state_vf_init_for_rectlist(struct ilo_state_vf *vf,
                               const struct ilo_dev *dev,
                               void *data, size_t data_size,
                               const struct ilo_state_vf_element_info *elements,
                               uint8_t element_count)
{
   struct ilo_state_vf_info info;

   memset(&info, 0, sizeof(info));

   info.data = data;
   info.data_size = data_size;

   info.elements = elements;
   info.element_count = element_count;

   /*
    * For VUE header,
    *
    *   DW0: Reserved: MBZ
    *   DW1: Render Target Array Index
    *   DW2: Viewport Index
    *   DW3: Point Width
    */
   info.params.prepend_zeros = true;

   return ilo_state_vf_init(vf, dev, &info);
}

bool
ilo_state_vf_set_params(struct ilo_state_vf *vf,
                        const struct ilo_dev *dev,
                        const struct ilo_state_vf_params_info *params)
{
   bool ret = true;

   ILO_DEV_ASSERT(dev, 6, 8);

   ret &= vf_params_set_gen6_internal_ve(vf, dev, params, vf->user_ve_count);
   if (ilo_dev_gen(dev) >= ILO_GEN(8))
      ret &= vf_params_set_gen8_3DSTATE_VF_SGVS(vf, dev, params);

   /*
    * From the Sandy Bridge PRM, volume 2 part 1, page 94:
    *
    *     "Edge flags are supported for the following primitive topology types
    *      only, otherwise EdgeFlagEnable must not be ENABLED.
    *
    *      - 3DPRIM_TRILIST*
    *      - 3DPRIM_TRISTRIP*
    *      - 3DPRIM_TRIFAN*
    *      - 3DPRIM_POLYGON"
    *
    *     "[DevSNB]: Edge Flags are not supported for QUADLIST primitives.
    *      Software may elect to convert QUADLIST primitives to some set of
    *      corresponding edge-flag-supported primitive types (e.g., POLYGONs)
    *      prior to submission to the 3D vf."
    *
    * From the Ivy Bridge PRM, volume 2 part 1, page 86:
    *
    *     "Edge flags are supported for all primitive topology types."
    *
    * Both PRMs are confusing...
    */
   if (params->last_element_edge_flag) {
      assert(vf->edge_flag_supported);
      if (ilo_dev_gen(dev) == ILO_GEN(6))
         assert(params->cv_topology != GEN6_3DPRIM_QUADLIST);
   }

   if (vf->edge_flag_supported) {
      assert(vf->user_ve_count);
      memcpy(vf->user_ve[vf->user_ve_count - 1],
            vf->last_user_ve[params->last_element_edge_flag],
            sizeof(vf->user_ve[vf->user_ve_count - 1]));
   }

   if (ilo_dev_gen(dev) >= ILO_GEN(7.5))
      ret &= vf_params_set_gen75_3DSTATE_VF(vf, dev, params);
   else
      ret &= vf_params_set_gen6_3dstate_index_buffer(vf, dev, params);

   assert(ret);

   return ret;
}

void
ilo_state_vf_full_delta(const struct ilo_state_vf *vf,
                        const struct ilo_dev *dev,
                        struct ilo_state_vf_delta *delta)
{
   delta->dirty = ILO_STATE_VF_3DSTATE_VERTEX_ELEMENTS;

   if (ilo_dev_gen(dev) >= ILO_GEN(8))
      delta->dirty |= ILO_STATE_VF_3DSTATE_VF_SGVS;

   if (ilo_dev_gen(dev) >= ILO_GEN(7.5))
      delta->dirty |= ILO_STATE_VF_3DSTATE_VF;
   else
      delta->dirty |= ILO_STATE_VF_3DSTATE_INDEX_BUFFER;
}

void
ilo_state_vf_get_delta(const struct ilo_state_vf *vf,
                       const struct ilo_dev *dev,
                       const struct ilo_state_vf *old,
                       struct ilo_state_vf_delta *delta)
{
   /* no shallow copying */
   assert(vf->user_ve != old->user_ve);

   delta->dirty = 0;

   if (vf->internal_ve_count != old->internal_ve_count ||
       vf->user_ve_count != old->user_ve_count ||
       memcmp(vf->internal_ve, old->internal_ve,
          sizeof(vf->internal_ve[0]) * vf->internal_ve_count) ||
       memcmp(vf->user_ve, old->user_ve,
          sizeof(vf->user_ve[0]) * vf->user_ve_count))
      delta->dirty |= ILO_STATE_VF_3DSTATE_VERTEX_ELEMENTS;

   if (ilo_dev_gen(dev) >= ILO_GEN(8)) {
      if (vf->sgvs[0] != old->sgvs[0])
         delta->dirty |= ILO_STATE_VF_3DSTATE_VF_SGVS;
   }

   if (ilo_dev_gen(dev) >= ILO_GEN(7.5)) {
      if (memcmp(vf->cut, old->cut, sizeof(vf->cut)))
         delta->dirty |= ILO_STATE_VF_3DSTATE_VF;
   } else {
      if (vf->cut[0] != old->cut[0])
         delta->dirty |= ILO_STATE_VF_3DSTATE_INDEX_BUFFER;
   }
}
