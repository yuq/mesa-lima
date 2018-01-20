/**************************************************************************
 *
 * Copyright 2013 Advanced Micro Devices, Inc.
 * All Rights Reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sub license, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice (including the
 * next paragraph) shall be included in all copies or substantial portions
 * of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT.
 * IN NO EVENT SHALL THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR
 * ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 **************************************************************************/

#include <tizplatform.h>
#include <tizkernel.h>
#include <tizscheduler.h>
#include <tizport.h>
#include <tizport_decls.h>
#include <tizvideoport.h>
#include <tizvideoport_decls.h>

#include "vid_dec_h264_common.h"
#include "entrypoint.h"
#include "h264d.h"
#include "h264dprc.h"
#include "h264dinport.h"
#include "names.h"

OMX_ERRORTYPE OMX_ComponentInit (OMX_HANDLETYPE ap_hdl)
{
   tiz_role_factory_t h264d_role;
   const tiz_role_factory_t * rf_list[] = {&h264d_role};
   tiz_type_factory_t h264dprc_type;
   tiz_type_factory_t h264d_inport_type;
   const tiz_type_factory_t * tf_list[] = {&h264dprc_type, &h264d_inport_type};

   /* Settings for roles */
   strcpy ((OMX_STRING) h264d_role.role, OMX_VID_DEC_AVC_ROLE);
   h264d_role.pf_cport = instantiate_h264d_config_port;
   h264d_role.pf_port[0] = instantiate_h264d_input_port;
   h264d_role.pf_port[1] = instantiate_h264d_output_port;
   h264d_role.nports = 2;
   h264d_role.pf_proc = instantiate_h264d_processor;

   /* Settings for classes */
   strcpy ((OMX_STRING) h264dprc_type.class_name, "h264dprc_class");
   h264dprc_type.pf_class_init = h264d_prc_class_init;
   strcpy ((OMX_STRING) h264dprc_type.object_name, "h264dprc");
   h264dprc_type.pf_object_init = h264d_prc_init;

   strcpy ((OMX_STRING) h264d_inport_type.class_name, "h264dinport_class");
   h264d_inport_type.pf_class_init = h264d_inport_class_init;
   strcpy ((OMX_STRING) h264d_inport_type.object_name, "h264dinport");
   h264d_inport_type.pf_object_init = h264d_inport_init;

   /* Initialize the component infrastructure */
   tiz_comp_init (ap_hdl, OMX_VID_COMP_NAME);

   /* Classes need to be registered first */
   tiz_comp_register_types (ap_hdl, tf_list, 2);

   /* Register the component roles */
   tiz_comp_register_roles (ap_hdl, rf_list, 1);

   return OMX_ErrorNone;
}
