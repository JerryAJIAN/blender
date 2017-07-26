/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifdef __OSL__
#  include "kernel/osl/osl_shader.h"
#endif

#include "kernel/kernel_random.h"
#include "kernel/kernel_projection.h"
#include "kernel/kernel_montecarlo.h"
#include "kernel/kernel_differential.h"
#include "kernel/kernel_camera.h"

#include "kernel/geom/geom.h"
#include "kernel/bvh/bvh.h"

#include "kernel/kernel_accumulate.h"
#include "kernel/kernel_shader.h"
#include "kernel/kernel_light.h"
#include "kernel/kernel_passes.h"

#ifdef __SUBSURFACE__
#  include "kernel/kernel_subsurface.h"
#endif

#ifdef __VOLUME__
#  include "kernel/kernel_volume.h"
#endif

#include "kernel_path_state.h"
#include "kernel_shadow.h"
#include "kernel_emission.h"
#include "kernel_path_common.h"
#include "kernel_path_surface.h"
#include "kernel_path_volume.h"
//#include "kernel_path_subsurface.h"

#ifdef __KERNEL_DEBUG__
#  include "kernel/kernel_debug.h"
#endif

CCL_NAMESPACE_BEGIN

ccl_device_noinline void kernel_path_ao(KernelGlobals *kg,
                                        ShaderData *sd,
                                        ShaderData *emission_sd,
                                        PathRadiance *L,
                                        PathState *state,
                                        RNG *rng,
                                        float3 throughput,
                                        float3 ao_alpha)
{
	/* todo: solve correlation */
	float bsdf_u, bsdf_v;

	path_state_rng_2D(kg, rng, state, PRNG_BSDF_U, &bsdf_u, &bsdf_v);

	float ao_factor = kernel_data.background.ao_factor;
	float3 ao_N;
	float3 ao_bsdf = shader_bsdf_ao(kg, sd, ao_factor, &ao_N);
	float3 ao_D;
	float ao_pdf;

	sample_cos_hemisphere(ao_N, bsdf_u, bsdf_v, &ao_D, &ao_pdf);

	if(dot(sd->Ng, ao_D) > 0.0f && ao_pdf != 0.0f) {
		Ray light_ray;
		float3 ao_shadow;

		light_ray.P = ray_offset(sd->P, sd->Ng);
		light_ray.D = ao_D;
		light_ray.t = kernel_data.background.ao_distance;
#ifdef __OBJECT_MOTION__
		light_ray.time = ccl_fetch(sd, time);
#endif
		light_ray.dP = ccl_fetch(sd, dP);
		light_ray.dD = differential3_zero();

        state->flag |= PATH_RAY_AO;

        uint shadow_linking = object_shadow_linking(kg, sd->object);
		if(!shadow_blocked(kg, emission_sd, state, &light_ray, &ao_shadow, shadow_linking))
			path_radiance_accum_ao(L, throughput, ao_alpha, ao_bsdf, ao_shadow, state->bounce);

        state->flag &= ~PATH_RAY_AO;
	}
}

ccl_device void kernel_path_indirect(KernelGlobals *kg,
                                     ShaderData *sd,
                                     ShaderData *emission_sd,
                                     RNG *rng,
                                     Ray *ray,
                                     float3 throughput,
                                     int num_samples,
                                     PathState *state,
                                     PathRadiance *L,
                                     uint light_linking)
{
	/* path iteration */
	for(;;) {
		/* intersect scene */
		Intersection isect;
		uint visibility = path_state_ray_visibility(kg, state);
		if(state->bounce > kernel_data.integrator.ao_bounces) {
			visibility = PATH_RAY_SHADOW;
			ray->t = kernel_data.background.ao_distance;
		}
		bool hit = scene_intersect(kg,
		                           *ray,
		                           visibility,
		                           &isect,
		                           NULL,
		                           0.0f, 0.0f,
                                   0x00000000/*TODO:What goes here*/);

#ifdef __LAMP_MIS__
		if(kernel_data.integrator.use_lamp_mis && !(state->flag & PATH_RAY_CAMERA)) {
			/* ray starting from previous non-transparent bounce */
			Ray light_ray;

			light_ray.P = ray->P - state->ray_t*ray->D;
			state->ray_t += isect.t;
			light_ray.D = ray->D;
			light_ray.t = state->ray_t;
			light_ray.time = ray->time;
			light_ray.dD = ray->dD;
			light_ray.dP = ray->dP;

			/* intersect with lamp */
			float3 emission;
			if(indirect_lamp_emission(kg, emission_sd, state, &light_ray, &emission, light_linking)) {
				path_radiance_accum_emission(L,
				                             throughput,
				                             emission,
				                             state->bounce);
			}
		}
#endif  /* __LAMP_MIS__ */

#ifdef __VOLUME__
		/* Sanitize volume stack. */
		if(!hit) {
			kernel_volume_clean_stack(kg, state->volume_stack);
		}
		/* volume attenuation, emission, scatter */
		if(state->volume_stack[0].shader != SHADER_NONE) {
			Ray volume_ray = *ray;
			volume_ray.t = (hit)? isect.t: FLT_MAX;

			bool heterogeneous =
			        volume_stack_is_heterogeneous(kg,
			                                      state->volume_stack);

#  ifdef __VOLUME_DECOUPLED__
			int sampling_method =
			        volume_stack_sampling_method(kg,
			                                     state->volume_stack);
			bool decoupled = kernel_volume_use_decoupled(kg, heterogeneous, false, sampling_method);

			if(decoupled) {
				/* cache steps along volume for repeated sampling */
				VolumeSegment volume_segment;

				shader_setup_from_volume(kg,
				                         sd,
				                         &volume_ray);
				kernel_volume_decoupled_record(kg,
				                               state,
				                               &volume_ray,
				                               sd,
				                               &volume_segment,
				                               heterogeneous);

				volume_segment.sampling_method = sampling_method;

				/* emission */
				if(volume_segment.closure_flag & SD_RUNTIME_EMISSION) {
					path_radiance_accum_emission(L,
					                             throughput,
					                             volume_segment.accum_emission,
					                             state->bounce);
				}

				/* scattering */
				VolumeIntegrateResult result = VOLUME_PATH_ATTENUATED;

				if(volume_segment.closure_flag & SD_RUNTIME_SCATTER) {
					int all = kernel_data.integrator.sample_all_lights_indirect;
                    uint light_linking = object_light_linking(kg, sd->object);
                    uint shadow_linking = object_shadow_linking(kg, sd->object);

					/* direct light sampling */
					kernel_branched_path_volume_connect_light(kg,
					                                          rng,
					                                          sd,
					                                          emission_sd,
					                                          throughput,
					                                          state,
					                                          L,
					                                          all,
					                                          &volume_ray,
					                                          &volume_segment,
                                                              light_linking,
                                                              shadow_linking);

					/* indirect sample. if we use distance sampling and take just
					 * one sample for direct and indirect light, we could share
					 * this computation, but makes code a bit complex */
					float rphase = path_state_rng_1D_for_decision(kg, rng, state, PRNG_PHASE);
					float rscatter = path_state_rng_1D_for_decision(kg, rng, state, PRNG_SCATTER_DISTANCE);

					result = kernel_volume_decoupled_scatter(kg,
					                                         state,
					                                         &volume_ray,
					                                         sd,
					                                         &throughput,
					                                         rphase,
					                                         rscatter,
					                                         &volume_segment,
					                                         NULL,
					                                         true);
				}

				/* free cached steps */
				kernel_volume_decoupled_free(kg, &volume_segment);

				if(result == VOLUME_PATH_SCATTERED) {
					if(kernel_path_volume_bounce(kg,
					                             rng,
					                             sd,
					                             &throughput,
					                             state,
					                             L,
					                             ray))
					{
						continue;
					}
					else {
						break;
					}
				}
				else {
					throughput *= volume_segment.accum_transmittance;
				}
			}
			else
#  endif  /* __VOLUME_DECOUPLED__ */
			{
				/* integrate along volume segment with distance sampling */
				VolumeIntegrateResult result = kernel_volume_integrate(
					kg, state, sd, &volume_ray, L, &throughput, rng, heterogeneous);

#  ifdef __VOLUME_SCATTER__
				if(result == VOLUME_PATH_SCATTERED) {
					/* direct lighting */
                    uint light_linking = object_light_linking(kg, sd->object);
                    uint shadow_linking = object_shadow_linking(kg, sd->object);

					kernel_path_volume_connect_light(kg,
					                                 rng,
					                                 sd,
					                                 emission_sd,
					                                 throughput,
					                                 state,
					                                 L,
                                                     light_linking,
                                                     shadow_linking);

					/* indirect light bounce */
					if(kernel_path_volume_bounce(kg,
					                             rng,
					                             sd,
					                             &throughput,
					                             state,
					                             L,
					                             ray))
					{
						continue;
					}
					else {
						break;
					}
				}
#  endif  /* __VOLUME_SCATTER__ */
			}
		}
#endif  /* __VOLUME__ */

		if(!hit) {
#ifdef __BACKGROUND__
			/* sample background shader */
			float3 L_background = indirect_background(kg, emission_sd, state, ray, NULL, 0);
			path_radiance_accum_background(L,
			                               state,
			                               throughput,
			                               L_background);
#endif  /* __BACKGROUND__ */

			break;
		}
		else if(state->bounce > kernel_data.integrator.ao_bounces) {
			break;
		}

		/* setup shading */
		shader_setup_from_ray(kg,
		                      sd,
		                      &isect,
		                      ray);
		float rbsdf = path_state_rng_1D_for_decision(kg, rng, state, PRNG_BSDF);
		shader_eval_surface(kg, sd, rng, state, rbsdf, state->flag, SHADER_CONTEXT_INDIRECT, NULL, 0);
#ifdef __BRANCHED_PATH__
		shader_merge_closures(sd);
#endif  /* __BRANCHED_PATH__ */

#ifdef __SHADOW_TRICKS__
		if(!(sd->object_flag & SD_OBJECT_OBJECT_SHADOW_CATCHER)) {
			state->flag &= ~PATH_RAY_SHADOW_CATCHER_ONLY;
		}
#endif  /* __SHADOW_TRICKS__ */

		/* blurring of bsdf after bounces, for rays that have a small likelihood
		 * of following this particular path (diffuse, rough glossy) */
		if(kernel_data.integrator.filter_glossy != FLT_MAX) {
			float blur_pdf = kernel_data.integrator.filter_glossy*state->min_ray_pdf;

			if(blur_pdf < 1.0f) {
				float blur_roughness = sqrtf(1.0f - blur_pdf)*0.5f;
				shader_bsdf_blur(kg, sd, blur_roughness);
			}
		}

#ifdef __EMISSION__
		/* emission */
		if(sd->runtime_flag & SD_RUNTIME_EMISSION) {
			float3 emission = indirect_primitive_emission(kg,
			                                              sd,
			                                              isect.t,
			                                              state->flag,
			                                              state->ray_pdf);
			path_radiance_accum_emission(L, throughput, emission, state->bounce);
		}
#endif  /* __EMISSION__ */

		/* path termination. this is a strange place to put the termination, it's
		 * mainly due to the mixed in MIS that we use. gives too many unneeded
		 * shader evaluations, only need emission if we are going to terminate */
		float probability =
		        path_state_terminate_probability(kg,
		                                         state,
												 sd,
		                                         throughput*num_samples);

		if(probability == 0.0f) {
			break;
		}
		else if(probability != 1.0f) {
			float terminate = path_state_rng_1D_for_decision(kg, rng, state, PRNG_TERMINATE);

			if(terminate >= probability)
				break;

			throughput /= probability;
		}

#ifdef __AO__
		/* ambient occlusion */
		if(kernel_data.integrator.use_ambient_occlusion || (sd->runtime_flag & SD_RUNTIME_AO)) {
			kernel_path_ao(kg, sd, emission_sd, L, state, rng, throughput, make_float3(0.0f, 0.0f, 0.0f));
		}
#endif  /* __AO__ */

#ifdef __SUBSURFACE__
		/* bssrdf scatter to a different location on the same object, replacing
		 * the closures with a diffuse BSDF */
		if(sd->runtime_flag & SD_RUNTIME_BSSRDF) {
			float bssrdf_probability;
			ShaderClosure *sc = subsurface_scatter_pick_closure(kg, sd, &bssrdf_probability);

			/* modify throughput for picking bssrdf or bsdf */
			throughput *= bssrdf_probability;

			/* do bssrdf scatter step if we picked a bssrdf closure */
			if(sc) {
				uint lcg_state = lcg_state_init(rng, state->rng_offset, state->sample, 0x68bc21eb);

				float bssrdf_u, bssrdf_v;
				path_state_rng_2D(kg,
				                  rng,
				                  state,
				                  PRNG_BSDF_U,
				                  &bssrdf_u, &bssrdf_v);
				subsurface_scatter_step(kg,
				                        sd,
				                        state,
				                        state->flag,
				                        sc,
				                        &lcg_state,
				                        bssrdf_u, bssrdf_v,
				                        false);
			}
		}
#endif  /* __SUBSURFACE__ */

#if defined(__EMISSION__) && defined(__BRANCHED_PATH__)
		if(kernel_data.integrator.use_direct_light) {
			int all = kernel_data.integrator.sample_all_lights_indirect;

            unsigned int light_linking = object_light_linking(kg, sd->object);
            unsigned int shadow_linking = object_shadow_linking(kg, sd->object);

			kernel_branched_path_surface_connect_light(kg,
			                                           rng,
			                                           sd,
			                                           emission_sd,
			                                           state,
			                                           throughput,
			                                           1.0f,
			                                           L,
			                                           all,
                                                       light_linking,
                                                       shadow_linking);
		}
#endif  /* defined(__EMISSION__) && defined(__BRANCHED_PATH__) */

		if(!kernel_path_surface_bounce(kg, rng, sd, &throughput, state, L, ray))
			break;
	}
}

#ifdef __SUBSURFACE__
#  ifndef __KERNEL_CUDA__
ccl_device
#  else
ccl_device_inline
#  endif
bool kernel_path_subsurface_scatter(
        KernelGlobals *kg,
        ShaderData *sd,
        ShaderData *emission_sd,
        PathRadiance *L,
        PathState *state,
        RNG *rng,
        Ray *ray,
        float3 *throughput,
        SubsurfaceIndirectRays *ss_indirect)
{
	float bssrdf_probability;
	ShaderClosure *sc = subsurface_scatter_pick_closure(kg, sd, &bssrdf_probability);

	/* modify throughput for picking bssrdf or bsdf */
	*throughput *= bssrdf_probability;

	/* do bssrdf scatter step if we picked a bssrdf closure */
	if(sc) {
		/* We should never have two consecutive BSSRDF bounces,
		 * the second one should be converted to a diffuse BSDF to
		 * avoid this.
		 */
		kernel_assert(!ss_indirect->tracing);

		uint lcg_state = lcg_state_init(rng, state->rng_offset, state->sample, 0x68bc21eb);

		SubsurfaceIntersection ss_isect;
		float bssrdf_u, bssrdf_v;
		path_state_rng_2D(kg, rng, state, PRNG_BSDF_U, &bssrdf_u, &bssrdf_v);
		int num_hits = subsurface_scatter_multi_intersect(kg,
		                                                  &ss_isect,
		                                                  sd,
		                                                  sc,
		                                                  &lcg_state,
		                                                  bssrdf_u, bssrdf_v,
		                                                  false);
#  ifdef __VOLUME__
		ss_indirect->need_update_volume_stack =
		        kernel_data.integrator.use_volumes &&
		        ccl_fetch(sd, object_flag) & SD_OBJECT_OBJECT_INTERSECTS_VOLUME;
#  endif

		/* compute lighting with the BSDF closure */
		for(int hit = 0; hit < num_hits; hit++) {
			/* NOTE: We reuse the existing ShaderData, we assume the path
			 * integration loop stops when this function returns true.
			 */
			subsurface_scatter_multi_setup(kg,
			                               &ss_isect,
			                               hit,
			                               sd,
			                               state,
			                               state->flag,
			                               sc,
			                               false);

			PathState *hit_state = &ss_indirect->state[ss_indirect->num_rays];
			Ray *hit_ray = &ss_indirect->rays[ss_indirect->num_rays];
			float3 *hit_tp = &ss_indirect->throughputs[ss_indirect->num_rays];
			PathRadiance *hit_L = &ss_indirect->L[ss_indirect->num_rays];

			*hit_state = *state;
			*hit_ray = *ray;
			*hit_tp = *throughput;

			hit_state->rng_offset += PRNG_BOUNCE_NUM;

			path_radiance_init(hit_L, kernel_data.film.use_light_pass);
			hit_L->direct_throughput = L->direct_throughput;
			path_radiance_copy_indirect(hit_L, L);

            uint light_linking = object_light_linking(kg, sd->object);
            uint shadow_linking = object_shadow_linking(kg, sd->object);
			kernel_path_surface_connect_light(kg, rng, sd, emission_sd, *hit_tp, state, hit_L, light_linking, shadow_linking);

			if(kernel_path_surface_bounce(kg,
			                              rng,
			                              sd,
			                              hit_tp,
			                              hit_state,
			                              hit_L,
			                              hit_ray))
			{
#  ifdef __LAMP_MIS__
				hit_state->ray_t = 0.0f;
#  endif  /* __LAMP_MIS__ */

#  ifdef __VOLUME__
				if(ss_indirect->need_update_volume_stack) {
					Ray volume_ray = *ray;
					/* Setup ray from previous surface point to the new one. */
					volume_ray.D = normalize_len(hit_ray->P - volume_ray.P,
					                             &volume_ray.t);

					kernel_volume_stack_update_for_subsurface(
					    kg,
					    emission_sd,
					    &volume_ray,
					    hit_state->volume_stack);
				}
#  endif  /* __VOLUME__ */
				path_radiance_reset_indirect(L);
				ss_indirect->num_rays++;
			}
			else {
				path_radiance_accum_sample(L, hit_L, 1);
			}
		}
		return true;
	}
	return false;
}

ccl_device_inline void kernel_path_subsurface_init_indirect(
        SubsurfaceIndirectRays *ss_indirect)
{
	ss_indirect->tracing = false;
	ss_indirect->num_rays = 0;
}

ccl_device void kernel_path_subsurface_accum_indirect(
        SubsurfaceIndirectRays *ss_indirect,
        PathRadiance *L)
{
	if(ss_indirect->tracing) {
		path_radiance_sum_indirect(L);
		path_radiance_accum_sample(&ss_indirect->direct_L, L, 1);
		if(ss_indirect->num_rays == 0) {
			*L = ss_indirect->direct_L;
		}
	}
}

ccl_device void kernel_path_subsurface_setup_indirect(
        KernelGlobals *kg,
        SubsurfaceIndirectRays *ss_indirect,
        PathState *state,
        Ray *ray,
        PathRadiance *L,
        float3 *throughput)
{
	if(!ss_indirect->tracing) {
		ss_indirect->direct_L = *L;
	}
	ss_indirect->tracing = true;

	/* Setup state, ray and throughput for indirect SSS rays. */
	ss_indirect->num_rays--;

	Ray *indirect_ray = &ss_indirect->rays[ss_indirect->num_rays];
	PathRadiance *indirect_L = &ss_indirect->L[ss_indirect->num_rays];

	*state = ss_indirect->state[ss_indirect->num_rays];
	*ray = *indirect_ray;
	*L = *indirect_L;
	*throughput = ss_indirect->throughputs[ss_indirect->num_rays];

	state->rng_offset += ss_indirect->num_rays * PRNG_BOUNCE_NUM;
}

#endif  /* __SUBSURFACE__ */

ccl_device_inline float4 kernel_path_integrate(KernelGlobals *kg,
                                               RNG *rng,
                                               int sample,
                                               Ray ray,
                                               ccl_global float *buffer)
{
	/* initialize */
	PathRadiance L;
	float3 throughput = make_float3(1.0f, 1.0f, 1.0f);
	float L_transparent = 0.0f;

	path_radiance_init(&L, kernel_data.film.use_light_pass);

	/* shader data memory used for both volumes and surfaces, saves stack space */
	ShaderData sd;
	/* shader data used by emission, shadows, volume stacks */
	ShaderData emission_sd;

	PathState state;
	path_state_init(kg, &emission_sd, &state, rng, sample, &ray);

#ifdef __KERNEL_DEBUG__
	DebugData debug_data;
	debug_data_init(&debug_data);
#endif  /* __KERNEL_DEBUG__ */

#ifdef __SUBSURFACE__
	SubsurfaceIndirectRays ss_indirect;
	kernel_path_subsurface_init_indirect(&ss_indirect);

	for(;;) {
#endif  /* __SUBSURFACE__ */

	/* path iteration */
	for(;;) {
		/* intersect scene */
		Intersection isect;
		uint visibility = path_state_ray_visibility(kg, &state);

#ifdef __HAIR__
		float difl = 0.0f, extmax = 0.0f;
		uint lcg_state = 0;

		if(kernel_data.bvh.have_curves) {
			if((kernel_data.cam.resolution == 1) && (state.flag & PATH_RAY_CAMERA)) {	
				float3 pixdiff = ray.dD.dx + ray.dD.dy;
				/*pixdiff = pixdiff - dot(pixdiff, ray.D)*ray.D;*/
				difl = kernel_data.curve.minimum_width * len(pixdiff) * 0.5f;
			}

			extmax = kernel_data.curve.maximum_width;
			lcg_state = lcg_state_init(rng, state.rng_offset, state.sample, 0x51633e2d);
		}

		bool hit = scene_intersect(kg, ray, visibility, &isect, &lcg_state, difl, extmax, 0x00000000/*TODO:What goes here*/);
#else
		bool hit = scene_intersect(kg, ray, visibility, &isect, NULL, 0.0f, 0.0f, 0x00000000/*TODO:What goes here*/);
#endif

#ifdef __KERNEL_DEBUG__
		if(state.flag & PATH_RAY_CAMERA) {
			debug_data.num_bvh_traversed_nodes += isect.num_traversed_nodes;
			debug_data.num_bvh_traversed_instances += isect.num_traversed_instances;
			debug_data.num_bvh_intersections += isect.num_intersections;
		}
		debug_data.num_ray_bounces++;
#endif  /* __KERNEL_DEBUG__ */

#ifdef __LAMP_MIS__
		if(kernel_data.integrator.use_lamp_mis && !(state.flag & PATH_RAY_CAMERA)) {
			/* ray starting from previous non-transparent bounce */
			Ray light_ray;

			light_ray.P = ray.P - state.ray_t*ray.D;
			state.ray_t += isect.t;
			light_ray.D = ray.D;
			light_ray.t = state.ray_t;
			light_ray.time = ray.time;
			light_ray.dD = ray.dD;
			light_ray.dP = ray.dP;

			/* intersect with lamp */
			float3 emission;

			if(indirect_lamp_emission(kg, &emission_sd, &state, &light_ray, &emission, 0x00000000/*TODO:What goes here*/))
				path_radiance_accum_emission(&L, throughput, emission, state.bounce);
		}
#endif  /* __LAMP_MIS__ */

#ifdef __VOLUME__
		/* Sanitize volume stack. */
		if(!hit) {
			kernel_volume_clean_stack(kg, state.volume_stack);
		}
		/* volume attenuation, emission, scatter */
		if(state.volume_stack[0].shader != SHADER_NONE) {
			Ray volume_ray = ray;
			volume_ray.t = (hit)? isect.t: FLT_MAX;

			bool heterogeneous = volume_stack_is_heterogeneous(kg, state.volume_stack);

#  ifdef __VOLUME_DECOUPLED__
			int sampling_method = volume_stack_sampling_method(kg, state.volume_stack);
			bool decoupled = kernel_volume_use_decoupled(kg, heterogeneous, true, sampling_method);

			if(decoupled) {
				/* cache steps along volume for repeated sampling */
				VolumeSegment volume_segment;

				shader_setup_from_volume(kg, &sd, &volume_ray);
				kernel_volume_decoupled_record(kg, &state,
					&volume_ray, &sd, &volume_segment, heterogeneous);

				volume_segment.sampling_method = sampling_method;

				/* emission */
				if(volume_segment.closure_flag & SD_RUNTIME_EMISSION)
					path_radiance_accum_emission(&L, throughput, volume_segment.accum_emission, state.bounce);

				/* scattering */
				VolumeIntegrateResult result = VOLUME_PATH_ATTENUATED;

				if(volume_segment.closure_flag & SD_RUNTIME_SCATTER) {
					int all = false;

                    uint light_linking = object_light_linking(kg, sd.object);
                    uint shadow_linking = object_shadow_linking(kg, sd.object);

					/* direct light sampling */
					kernel_branched_path_volume_connect_light(kg, rng, &sd,
						&emission_sd, throughput, &state, &L, all,
						&volume_ray, &volume_segment, light_linking, shadow_linking);

					/* indirect sample. if we use distance sampling and take just
					 * one sample for direct and indirect light, we could share
					 * this computation, but makes code a bit complex */
					float rphase = path_state_rng_1D_for_decision(kg, rng, &state, PRNG_PHASE);
					float rscatter = path_state_rng_1D_for_decision(kg, rng, &state, PRNG_SCATTER_DISTANCE);

					result = kernel_volume_decoupled_scatter(kg,
						&state, &volume_ray, &sd, &throughput,
						rphase, rscatter, &volume_segment, NULL, true);
				}

				/* free cached steps */
				kernel_volume_decoupled_free(kg, &volume_segment);

				if(result == VOLUME_PATH_SCATTERED) {
					if(kernel_path_volume_bounce(kg, rng, &sd, &throughput, &state, &L, &ray))
						continue;
					else
						break;
				}
				else {
					throughput *= volume_segment.accum_transmittance;
				}
			}
			else
#  endif  /* __VOLUME_DECOUPLED__ */
			{
				/* integrate along volume segment with distance sampling */
				VolumeIntegrateResult result = kernel_volume_integrate(
					kg, &state, &sd, &volume_ray, &L, &throughput, rng, heterogeneous);

#  ifdef __VOLUME_SCATTER__
				if(result == VOLUME_PATH_SCATTERED) {
					/* direct lighting */
                    uint light_linking = object_light_linking(kg, sd.object);
                    uint shadow_linking = object_shadow_linking(kg, sd.object);

					kernel_path_volume_connect_light(kg, rng, &sd, &emission_sd, throughput, &state, &L, light_linking, shadow_linking);

					/* indirect light bounce */
					if(kernel_path_volume_bounce(kg, rng, &sd, &throughput, &state, &L, &ray))
						continue;
					else
						break;
				}
#  endif  /* __VOLUME_SCATTER__ */
			}
		}
#endif  /* __VOLUME__ */

		if(!hit) {
			/* eval background shader if nothing hit */
			if(kernel_data.background.transparent && (state.flag & PATH_RAY_CAMERA)) {
				L_transparent += average(throughput);

#ifdef __PASSES__
				if(!(kernel_data.film.pass_flag & PASS_BACKGROUND))
#endif  /* __PASSES__ */
					break;
			}

#ifdef __BACKGROUND__
			/* sample background shader */
			float3 L_background = indirect_background(kg, &emission_sd, &state, &ray, buffer, sample);
			path_radiance_accum_background(&L, &state, throughput, L_background);
#endif  /* __BACKGROUND__ */

			break;
		}
		else if(state.bounce > kernel_data.integrator.ao_bounces) {
			break;
		}

		/* setup shading */
		shader_setup_from_ray(kg, &sd, &isect, &ray);
		float rbsdf = path_state_rng_1D_for_decision(kg, rng, &state, PRNG_BSDF);
		shader_eval_surface(kg, &sd, rng, &state, rbsdf, state.flag, SHADER_CONTEXT_MAIN, buffer, sample);

#ifdef __SHADOW_TRICKS__
		if((sd.object_flag & SD_OBJECT_OBJECT_SHADOW_CATCHER)) {
			if(state.flag & PATH_RAY_CAMERA) {
				state.flag |= (PATH_RAY_SHADOW_CATCHER | PATH_RAY_SHADOW_CATCHER_ONLY);
				state.catcher_object = sd.object;
				if(!kernel_data.background.transparent) {
					L.shadow_color = indirect_background(kg, &emission_sd, &state, &ray, buffer, sample);
				}
			}
		}
		else {
			state.flag &= ~PATH_RAY_SHADOW_CATCHER_ONLY;
		}
#endif  /* __SHADOW_TRICKS__ */

		/* holdout */
#ifdef __HOLDOUT__
		if(((sd.runtime_flag & SD_RUNTIME_HOLDOUT) || (sd.object_flag & SD_OBJECT_HOLDOUT_MASK)) && (state.flag & PATH_RAY_CAMERA)) {
			if(kernel_data.background.transparent) {
				float3 holdout_weight;
				
				if(sd.object_flag & SD_OBJECT_HOLDOUT_MASK) {
					holdout_weight = make_float3(1.0f, 1.0f, 1.0f);
				}
				else {
					holdout_weight = shader_holdout_eval(kg, &sd);
				}
				/* any throughput is ok, should all be identical here */
				L_transparent += average(holdout_weight*throughput);
			}

			if (sd.object_flag & SD_OBJECT_HOLDOUT_MASK) {
				break;
			}
		}
#endif  /* __HOLDOUT__ */

		/* holdout mask objects do not write data passes */
		kernel_write_data_passes(kg, buffer, &L, &sd, sample, &state, throughput);

		/* blurring of bsdf after bounces, for rays that have a small likelihood
		 * of following this particular path (diffuse, rough glossy) */
		if(kernel_data.integrator.filter_glossy != FLT_MAX) {
			float blur_pdf = kernel_data.integrator.filter_glossy*state.min_ray_pdf;

			if(blur_pdf < 1.0f) {
				float blur_roughness = sqrtf(1.0f - blur_pdf)*0.5f;
				shader_bsdf_blur(kg, &sd, blur_roughness);
			}
		}

#ifdef __EMISSION__
		/* emission */
		if(sd.runtime_flag & SD_RUNTIME_EMISSION) {
			/* todo: is isect.t wrong here for transparent surfaces? */
			float3 emission = indirect_primitive_emission(kg, &sd, isect.t, state.flag, state.ray_pdf);
			path_radiance_accum_emission(&L, throughput, emission, state.bounce);
		}
#endif  /* __EMISSION__ */

		/* path termination. this is a strange place to put the termination, it's
		 * mainly due to the mixed in MIS that we use. gives too many unneeded
		 * shader evaluations, only need emission if we are going to terminate */
		float probability = path_state_terminate_probability(kg, &state, &sd, throughput);

		if(probability == 0.0f) {
			break;
		}
		else if(probability != 1.0f) {
			float terminate = path_state_rng_1D_for_decision(kg, rng, &state, PRNG_TERMINATE);
			if(terminate >= probability)
				break;

			throughput /= probability;
		}

#ifdef __AO__
		/* ambient occlusion */
		if(kernel_data.integrator.use_ambient_occlusion || (sd.runtime_flag & SD_RUNTIME_AO)) {
			kernel_path_ao(kg, &sd, &emission_sd, &L, &state, rng, throughput, shader_bsdf_alpha(kg, &sd));
		}
#endif  /* __AO__ */

#ifdef __SUBSURFACE__
		/* bssrdf scatter to a different location on the same object, replacing
		 * the closures with a diffuse BSDF */
		if(sd.runtime_flag & SD_RUNTIME_BSSRDF) {
			if(kernel_path_subsurface_scatter(kg,
			                                  &sd,
			                                  &emission_sd,
			                                  &L,
			                                  &state,
			                                  rng,
			                                  &ray,
			                                  &throughput,
			                                  &ss_indirect))
			{
				break;
			}
		}
#endif  /* __SUBSURFACE__ */

		/* direct lighting */
        uint light_linking = object_light_linking(kg, sd.object);
        uint shadow_linking = object_shadow_linking(kg, sd.object);

		kernel_path_surface_connect_light(kg, rng, &sd, &emission_sd, throughput, &state, &L, light_linking, shadow_linking);

		/* compute direct lighting and next bounce */
		if(!kernel_path_surface_bounce(kg, rng, &sd, &throughput, &state, &L, &ray))
			break;
	}

#ifdef __SUBSURFACE__
		kernel_path_subsurface_accum_indirect(&ss_indirect, &L);

		/* Trace indirect subsurface rays by restarting the loop. this uses less
		 * stack memory than invoking kernel_path_indirect.
		 */
		if(ss_indirect.num_rays) {
			kernel_path_subsurface_setup_indirect(kg,
			                                      &ss_indirect,
			                                      &state,
			                                      &ray,
			                                      &L,
			                                      &throughput);
		}
		else {
			break;
		}
	}
#endif  /* __SUBSURFACE__ */

	float3 L_sum;
#ifdef __SHADOW_TRICKS__
	if(state.flag & PATH_RAY_SHADOW_CATCHER) {
		L_sum = path_radiance_sum_shadowcatcher(kg, &L, &L_transparent);
	}
	else
#endif  /* __SHADOW_TRICKS__ */
	{
		L_sum = path_radiance_clamp_and_sum(kg, &L);
	}

	kernel_write_light_passes(kg, buffer, &L, sample);

#ifdef __KERNEL_DEBUG__
	kernel_write_debug_passes(kg, buffer, &state, &debug_data, sample);
#endif  /* __KERNEL_DEBUG__ */

	return make_float4(L_sum.x, L_sum.y, L_sum.z, 1.0f - L_transparent);
}

ccl_device void kernel_path_trace(KernelGlobals *kg,
	ccl_global float *buffer, ccl_global uint *rng_state,
	int sample, int x, int y, int offset, int stride)
{
	/* buffer offset */
	int index = offset + x + y*stride;
	int pass_stride = kernel_data.film.pass_stride;

	rng_state += index;
	buffer += index*pass_stride;

	/* initialize random numbers and ray */
	RNG rng;
	Ray ray;

	kernel_path_trace_setup(kg, rng_state, sample, x, y, &rng, &ray);

	/* integrate */
	float4 L;

	if(ray.t != 0.0f)
		L = kernel_path_integrate(kg, &rng, sample, ray, buffer);
	else
		L = make_float4(0.0f, 0.0f, 0.0f, 0.0f);

	/* accumulate result in output buffer */
	kernel_write_pass_float4(buffer, sample, L);

	path_rng_end(kg, rng_state, rng);
}

CCL_NAMESPACE_END

