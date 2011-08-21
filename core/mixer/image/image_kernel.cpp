/*
* copyright (c) 2010 Sveriges Television AB <info@casparcg.com>
*
*  This file is part of CasparCG.
*
*    CasparCG is free software: you can redistribute it and/or modify
*    it under the terms of the GNU General Public License as published by
*    the Free Software Foundation, either version 3 of the License, or
*    (at your option) any later version.
*
*    CasparCG is distributed in the hope that it will be useful,
*    but WITHOUT ANY WARRANTY; without even the implied warranty of
*    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
*    GNU General Public License for more details.

*    You should have received a copy of the GNU General Public License
*    along with CasparCG.  If not, see <http://www.gnu.org/licenses/>.
*
*/
#include "../../stdafx.h"

#include "image_kernel.h"

#include "image_shader.h"

#include "blending_glsl.h"
#include "../gpu/shader.h"
#include "../gpu/device_buffer.h"
#include "../gpu/ogl_device.h"

#include <common/exception/exceptions.h>
#include <common/gl/gl_check.h>
#include <common/env.h>

#include <core/video_format.h>
#include <core/producer/frame/pixel_format.h>
#include <core/producer/frame/frame_transform.h>

#include <GL/glew.h>

#include <boost/noncopyable.hpp>

#include <unordered_map>

namespace caspar { namespace core {
	
GLubyte upper_pattern[] = {
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,
	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00};
		
GLubyte lower_pattern[] = {
	0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 
	0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff,
	0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff,	0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff,
	0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff,	0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00,	0xff, 0xff, 0xff, 0xff};

struct image_kernel::implementation : boost::noncopyable
{	
	std::shared_ptr<shader>	shader_;
	bool					blend_modes_;
							
	void draw(ogl_device&									ogl,
			  render_item&&									item,
			  device_buffer&								background,
			  const std::shared_ptr<device_buffer>&			local_key,			  
			  const std::shared_ptr<device_buffer>&			layer_key)
	{
		static const double epsilon = 0.001;

		CASPAR_ASSERT(item.pix_desc.planes.size() == item.textures.size());

		if(item.textures.empty())
			return;

		if(item.transform.opacity < epsilon)
			return;
		
		if(!std::all_of(item.textures.begin(), item.textures.end(), std::mem_fn(&device_buffer::ready)))
		{
			CASPAR_LOG(warning) << L"[image_mixer] Performance warning. Host to device transfer not complete, GPU will be stalled";
			ogl.yield(); // Try to give it some more time.
		}		
		
		// Bind textures

		for(size_t n = 0; n < item.textures.size(); ++n)
			item.textures[n]->bind(n);

		if(local_key)
			local_key->bind(texture_id::local_key);
		
		if(layer_key)
			layer_key->bind(texture_id::layer_key);
			
		// Setup shader

		if(!shader_)
			shader_ = get_image_shader(ogl, blend_modes_);
						
		ogl.use(*shader_);

		shader_->set("plane[0]",		texture_id::plane0);
		shader_->set("plane[1]",		texture_id::plane1);
		shader_->set("plane[2]",		texture_id::plane2);
		shader_->set("plane[3]",		texture_id::plane3);
		shader_->set("local_key",		texture_id::local_key);
		shader_->set("layer_key",		texture_id::layer_key);
		shader_->set("is_hd",		 	item.pix_desc.planes.at(0).height > 700 ? 1 : 0);
		shader_->set("has_local_key",	local_key);
		shader_->set("has_layer_key",	layer_key);
		shader_->set("pixel_format",	item.pix_desc.pix_fmt);	
		shader_->set("opacity",			item.transform.is_key ? 1.0 : item.transform.opacity);	
		
		// Setup blend_func
		
		if(item.transform.is_key)
			item.blend_mode = blend_mode::normal;

		if(blend_modes_)
		{
			background.bind(6);

			shader_->set("background",	texture_id::background);
			shader_->set("blend_mode",	item.blend_mode);
		}
		else
		{
			switch(item.blend_mode)
			{
			case blend_mode::replace:			
				ogl.blend_func_separate(GL_ONE, GL_ZERO, GL_ONE, GL_ONE);
				break;
			case blend_mode::normal:
			default:
				ogl.blend_func_separate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
				break;
			}
		}

		// Setup image-adjustements
		
		if(item.transform.levels.min_input  > epsilon		||
		   item.transform.levels.max_input  < 1.0-epsilon	||
		   item.transform.levels.min_output > epsilon		||
		   item.transform.levels.max_output < 1.0-epsilon	||
		   std::abs(item.transform.levels.gamma - 1.0) > epsilon)
		{
			shader_->set("levels", true);	
			shader_->set("min_input",	item.transform.levels.min_input);	
			shader_->set("max_input",	item.transform.levels.max_input);
			shader_->set("min_output",	item.transform.levels.min_output);
			shader_->set("max_output",	item.transform.levels.max_output);
			shader_->set("gamma",		item.transform.levels.gamma);
		}
		else
			shader_->set("levels", false);	

		if(std::abs(item.transform.brightness - 1.0) > epsilon ||
		   std::abs(item.transform.saturation - 1.0) > epsilon ||
		   std::abs(item.transform.contrast - 1.0)   > epsilon)
		{
			shader_->set("csb",	true);	
			
			shader_->set("brt", item.transform.brightness);	
			shader_->set("sat", item.transform.saturation);
			shader_->set("con", item.transform.contrast);
		}
		else
			shader_->set("csb",	false);	
		
		// Setup interlacing

		if(item.transform.field_mode == core::field_mode::progressive)			
			ogl.disable(GL_POLYGON_STIPPLE);			
		else			
		{
			ogl.enable(GL_POLYGON_STIPPLE);

			if(item.transform.field_mode == core::field_mode::upper)
				ogl.stipple_pattern(upper_pattern);
			else if(item.transform.field_mode == core::field_mode::lower)
				ogl.stipple_pattern(lower_pattern);
		}

		// Setup drawing area
		
		ogl.viewport(0, 0, background.width(), background.height());
								
		auto m_p = item.transform.clip_translation;
		auto m_s = item.transform.clip_scale;

		bool scissor = m_p[0] > std::numeric_limits<double>::epsilon()			|| m_p[1] > std::numeric_limits<double>::epsilon() ||
					   m_s[0] < (1.0 - std::numeric_limits<double>::epsilon())	|| m_s[1] < (1.0 - std::numeric_limits<double>::epsilon());

		if(scissor)
		{
			double w = static_cast<double>(background.width());
			double h = static_cast<double>(background.height());
		
			ogl.enable(GL_SCISSOR_TEST);
			ogl.scissor(static_cast<size_t>(m_p[0]*w), static_cast<size_t>(m_p[1]*h), static_cast<size_t>(m_s[0]*w), static_cast<size_t>(m_s[1]*h));
		}

		auto f_p = item.transform.fill_translation;
		auto f_s = item.transform.fill_scale;
		
		// Set render target
		
		ogl.attach(background);
		
		// Draw

		glBegin(GL_QUADS);
			glMultiTexCoord2d(GL_TEXTURE0, 0.0, 0.0); glMultiTexCoord2d(GL_TEXTURE1,  f_p[0]        ,  f_p[1]        );		glVertex2d( f_p[0]        *2.0-1.0,  f_p[1]        *2.0-1.0);
			glMultiTexCoord2d(GL_TEXTURE0, 1.0, 0.0); glMultiTexCoord2d(GL_TEXTURE1, (f_p[0]+f_s[0]),  f_p[1]        );		glVertex2d((f_p[0]+f_s[0])*2.0-1.0,  f_p[1]        *2.0-1.0);
			glMultiTexCoord2d(GL_TEXTURE0, 1.0, 1.0); glMultiTexCoord2d(GL_TEXTURE1, (f_p[0]+f_s[0]), (f_p[1]+f_s[1]));		glVertex2d((f_p[0]+f_s[0])*2.0-1.0, (f_p[1]+f_s[1])*2.0-1.0);
			glMultiTexCoord2d(GL_TEXTURE0, 0.0, 1.0); glMultiTexCoord2d(GL_TEXTURE1,  f_p[0]        , (f_p[1]+f_s[1]));		glVertex2d( f_p[0]        *2.0-1.0, (f_p[1]+f_s[1])*2.0-1.0);
		glEnd();

		// Cleanup

		ogl.disable(GL_SCISSOR_TEST);	
				
		item.textures.clear();
		ogl.yield(); // Return resources to pool as early as possible.

		if(blend_modes_)
		{
			// http://www.opengl.org/registry/specs/NV/texture_barrier.txt
			// This allows us to use framebuffer (background) both as source and target while blending.
			glTextureBarrierNV(); 
		}
	}
};

image_kernel::image_kernel() : impl_(new implementation()){}
void image_kernel::draw(ogl_device& ogl, 
						render_item&& item, 
						device_buffer& background,
						const std::shared_ptr<device_buffer>& local_key, 
						const std::shared_ptr<device_buffer>& layer_key)
{
	impl_->draw(ogl, std::move(item), background, local_key, layer_key);
}

bool operator==(const render_item& lhs, const render_item& rhs)
{
	return lhs.textures == rhs.textures && lhs.transform == rhs.transform;
}

}}