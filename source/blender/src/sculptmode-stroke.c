/*
 * $Id$
 *
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 *
 * The Original Code is Copyright (C) 2007 by Nicholas Bishop
 * All rights reserved.
 *
 * The Original Code is: all of this file.
 *
 * Contributor(s): none yet.
 *
 * ***** END GPL LICENSE BLOCK *****
 *
 * Storage and manipulation of sculptmode brush strokes.
 *
 */

#include "MEM_guardedalloc.h"
#include "DNA_listBase.h"
#include "BLI_blenlib.h"
#include "BIF_gl.h"
#include "BDR_sculptmode.h"
#include <math.h>

/* Temporary storage of input stroke control points */
typedef struct StrokePoint {
	struct StrokePoint *next, *prev;
	short x, y;
} StrokePoint;
typedef struct SculptStroke {
	short (*loc)[2];
	int max;
	int index;
	float length;
	ListBase final;
	StrokePoint *final_mem;
} SculptStroke;

void sculpt_stroke_new(const int max)
{
	SculptSession *ss = sculpt_session();

	ss->stroke = MEM_callocN(sizeof(SculptStroke), "SculptStroke");
	ss->stroke->loc = MEM_callocN(sizeof(short) * 2 * max, "SculptStroke.loc");
	ss->stroke->max = max;
	ss->stroke->index = -1;
}

void sculpt_stroke_free()
{
	SculptSession *ss = sculpt_session();
	if(ss && ss->stroke) {
		if(ss->stroke->loc) MEM_freeN(ss->stroke->loc);
		if(ss->stroke->final_mem) MEM_freeN(ss->stroke->final_mem);

		MEM_freeN(ss->stroke);
		ss->stroke = NULL;
	}
}

void sculpt_stroke_add_point(const short x, const short y)
{
	SculptStroke *stroke = sculpt_session()->stroke;
	const int next = stroke->index + 1;

	if(stroke->index == -1) {
		stroke->loc[0][0] = x;
		stroke->loc[0][1] = y;
		stroke->index = 0;
	}
	else if(next < stroke->max) {
		const int dx = x - stroke->loc[stroke->index][0];
		const int dy = y - stroke->loc[stroke->index][1];
		stroke->loc[next][0] = x;
		stroke->loc[next][1] = y;
		stroke->length += sqrt(dx*dx + dy*dy);
		stroke->index = next;
	}
}

void sculpt_stroke_smooth(SculptStroke *stroke)
{
	/* Apply smoothing (exclude the first and last points)*/
	StrokePoint *p = stroke->final.first;
	if(p && p->next && p->next->next) {
		for(p = p->next->next; p && p->next && p->next->next; p = p->next) {
			p->x = p->prev->prev->x*0.1 + p->prev->x*0.2 + p->x*0.4 + p->next->x*0.2 + p->next->next->x*0.1;
			p->y = p->prev->prev->y*0.1 + p->prev->y*0.2 + p->y*0.4 + p->next->y*0.2 + p->next->next->y*0.1;
		}
	}	
}

void sculpt_stroke_create_final()
{
	SculptStroke *stroke = sculpt_session()->stroke;

	if(stroke) {
		StrokePoint *p, *pnext;
		int i;

		/* Copy loc into final */
		if(stroke->final_mem)
			MEM_freeN(stroke->final_mem);
		stroke->final_mem = MEM_callocN(sizeof(StrokePoint) * (stroke->index + 1) * 2, "SculptStroke.final");
		stroke->final.first = stroke->final.last = NULL;
		for(i = 0; i <= stroke->index; ++i) {
			p = &stroke->final_mem[i];
			p->x = stroke->loc[i][0];
			p->y = stroke->loc[i][1];
			BLI_addtail(&stroke->final, p);
		}

		/* Remove shortest edges */
		for(p = ((StrokePoint*)stroke->final.first)->next; p && p->next; p = pnext) {
			const int dx = p->x - p->prev->x;
			const int dy = p->y - p->prev->y;
			const float len = sqrt(dx*dx + dy*dy);
			pnext = p->next;
			if(len < 10) {
				BLI_remlink(&stroke->final, p);
			}
		}

		sculpt_stroke_smooth(stroke);

		/* Subdivide edges */
		for(p = stroke->final.first; p && p->next; p = pnext) {
			StrokePoint *np = &stroke->final_mem[i++];

			pnext = p->next;
			np->x = (p->x + p->next->x) / 2;
			np->y = (p->y + p->next->y) / 2;
			BLI_insertlink(&stroke->final, p, np);
		}

		sculpt_stroke_smooth(stroke);
	}
}

float sculpt_stroke_seglen(StrokePoint *p1, StrokePoint *p2)
{
	int dx = p2->x - p1->x;
	int dy = p2->y - p1->y;
	return sqrt(dx*dx + dy*dy);
}

void sculpt_stroke_apply(struct EditData *e)
{
	SculptStroke *stroke = sculpt_session()->stroke;

	if(stroke) {
		sculpt_stroke_create_final();

		if(stroke->length > 200) {
			const short spacing = 2;
			const int dots = stroke->length / spacing;
			int i;
			StrokePoint *p = stroke->final.first;
			float startloc = 0;

			for(i = 0; i < dots && p && p->next; ++i) {
				const float dotloc = spacing * i;
				short co[2];
				float len = sculpt_stroke_seglen(p, p->next);
				float u, v;
				
				/* Find edge containing dot */
				while(dotloc > startloc + len && p && p->next && p->next->next) {
					p = p->next;
					startloc += len;
					len = sculpt_stroke_seglen(p, p->next);
				}

				if(!p || !p->next) break;


				u = (dotloc - startloc) / len;
				v = 1 - u;
					
				co[0] = p->x*u + p->next->x*v;
				co[1] = p->y*u + p->next->y*v;

				if(startloc > 100)
					break;

				do_symmetrical_brush_actions(e, co, NULL);
			}

			/* Replace remaining values in stroke->loc with remaining stroke->final values */
			stroke->index = -1;
			stroke->length = 0;
			for(; p; p = p->next) {
				++stroke->index;
				stroke->loc[stroke->index][0] = p->x;
				stroke->loc[stroke->index][1] = p->y;
				if(p->next) {
					stroke->length += sculpt_stroke_seglen(p, p->next);
				}
			}
		}
	}
}

void sculpt_stroke_apply_all(struct EditData *e)
{
	SculptStroke *stroke = sculpt_session()->stroke;

	sculpt_stroke_create_final();

	if(stroke) {
		const short spacing = 2;
		const int dots = stroke->length / spacing;
		int i;
		StrokePoint *p = stroke->final.first;
		float startloc = 0;

		for(i = 0; i < dots && p && p->next; ++i) {
			const float dotloc = spacing * i;
			short co[2];
			float len = sculpt_stroke_seglen(p, p->next);
			float u, v;
				
			/* Find edge containing dot */
			while(dotloc > startloc + len && p && p->next && p->next->next) {
				p = p->next;
				startloc += len;
				len = sculpt_stroke_seglen(p, p->next);
			}

			if(!p || !p->next) break;


			u = (dotloc - startloc) / len;
			v = 1 - u;
					
			co[0] = p->x*u + p->next->x*v;
			co[1] = p->y*u + p->next->y*v;

			do_symmetrical_brush_actions(e, co, NULL);
		}
	}
}

void sculpt_stroke_draw()
{
	SculptStroke *stroke = sculpt_session()->stroke;

	if(stroke) {
		StrokePoint *p;

		/* Draws the original stroke */
		/*glColor3f(1, 0, 0);		
		glBegin(GL_LINE_STRIP);
		for(i = 0; i <= stroke->index; ++i)
			glVertex2s(stroke->loc[i][0], stroke->loc[i][1]);
		glEnd();*/

		/* Draws the smoothed stroke */
		glColor3f(0, 1, 0);
		glBegin(GL_LINE_STRIP);
		for(p = stroke->final.first; p; p = p->next)
			glVertex2s(p->x, p->y);
		glEnd();
	}
}
