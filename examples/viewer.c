/* Pango
 * viewer.c: Example program to view a UTF-8 encoding file
 *           using Pango to render result.
 *
 * Copyright (C) 1999 Red Hat Software
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#include <gtk/gtk.h>
#include <gdk/gdkx.h>

#include "pango.h"
#include "pangox.h"

#include <unicode.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "utils.h"

#define BUFSIZE 1024

typedef struct _Paragraph Paragraph;
typedef struct _Line Line;

/* Structure representing a paragraph
 */
struct _Paragraph {
  char *text;
  int length;
  int height;   /* Height, in pixels */
  PangoLayout *layout;
};

/* Structure representing a line
 */
 struct _Line {
  /* List of PangoItems for this paragraph in visual order */
  GList *runs;
  int ascent;   /* Ascent of line, in pixels */
  int descent;  /* Descent of lines, in pixels */
  int offset;   /* Offset from left margin line, in pixels */
};

static PangoFontDescription font_description;
static Paragraph *highlight_para;
static int highlight_offset;

GtkWidget *styles_combo;

static GtkWidget *message_label;
GtkWidget *layout;

PangoContext *context;

static void fill_styles_combo (GtkWidget *combo);

/* Read an entire file into a string
 */
static char *
read_file(char *name)
{
  GString *inbuf;
  int fd;
  char *text;
  char buffer[BUFSIZE];

  fd = open(name, O_RDONLY);
  if (!fd)
    {
      fprintf(stderr, "gscript-viewer: Cannot open %s: %s\n",
	      name, g_strerror (errno));
      return NULL;
    }

  inbuf = g_string_new (NULL);
  while (1)
    {
      int count = read (fd, buffer, BUFSIZE-1);
      if (count < 0)
	{
	  fprintf(stderr, "gscript-viewer: Error reading %s: %s\n",
		  name, g_strerror (errno));
	  g_string_free (inbuf, TRUE);
	  return NULL;
	}
      else if (count == 0)
	break;

      buffer[count] = '\0';

      g_string_append (inbuf, buffer);
    }

  close (fd);

  text = inbuf->str;
  g_string_free (inbuf, FALSE);

  return text;
}

/* Take a UTF8 string and break it into paragraphs on \n characters
 */
static GList *
split_paragraphs (char *text)
{
  char *p = text;
  GUChar4 wc;
  GList *result = NULL;
  char *last_para = text;
  
  while (*p)
    {
      char *next = unicode_get_utf8 (p, &wc);
      if (!next)
	{
	  fprintf (stderr, "gscript-viewer: Invalid character in input\n");
	  g_list_foreach (result, (GFunc)g_free, NULL);
	  return NULL;
	}
      if (!*p || !wc || wc == '\n')
	{
	  Paragraph *para = g_new (Paragraph, 1);
	  para->text = last_para;
	  para->length = p - last_para;
	  para->height = 0;
	  para->layout = pango_layout_new (context);
	  pango_layout_set_text (para->layout, para->text, para->length);
	  
	  last_para = next;
	    
	  result = g_list_prepend (result, para);
	}
      if (!wc) /* incomplete character at end */
	break;
      p = next;
    }

  return g_list_reverse (result);
}

/* Break a paragraph into a list of lines which fit into
 * width, and compute the total height of the new paragraph
 */
void
layout_paragraph (Paragraph *para, int width)
{
  GSList *line_list;
  PangoRectangle logical_rect;
  int height = 0;
  
  pango_layout_set_width (para->layout, width * 1000);
  pango_layout_set_first_line_width (para->layout, width * 1000);

  line_list = pango_layout_get_lines (para->layout);
  while (line_list)
    {
      PangoLayoutLine *line = line_list->data;
      line_list = line_list->next;

      pango_layout_line_get_extents (line, NULL, &logical_rect);
      height += logical_rect.height;
    }

  para->height = height / 1000;
}

/* Given an x-y position, return the paragraph and offset
 * within the paragraph of the click.
 */
gboolean
xy_to_cp (GList *paragraphs, int width, int x, int y,
	  Paragraph **para_return, int *offset)
{
  GList *para_list;
  int height = 0;
  PangoDirection base_dir = pango_context_get_base_dir (context);

  *para_return = NULL;

  para_list = paragraphs;
  while (para_list && height < y)
    {
      Paragraph *para = para_list->data;
      
      if (height + para->height >= y)
	{
	  PangoRectangle logical_rect;
	  GSList *line_list = pango_layout_get_lines (para->layout);
	  
	  while (line_list)
	    {
	      PangoLayoutLine *line = line_list->data;
	      int line_y = (y - height) * 1000;
	      int line_height = 0;

	      pango_layout_line_get_extents (line_list->data, NULL, &logical_rect);
	      
	      if (line_height + logical_rect.height >= line_y)
		{
		  int x_offset;

		  if (base_dir == PANGO_DIRECTION_RTL)
		    x_offset = width - logical_rect.width / 1000;
		  else
		    x_offset = 0;
		  
		  if (pango_layout_line_x_to_index (line, 1000 * (x - x_offset), offset, NULL))
		    {
		      *para_return = para;
		      return TRUE;
		    }
		  else
		    return FALSE;
		}

	      line_height += logical_rect.height;
	      line_list = line_list->next;
	    }
	}
      
      height += para->height;
      para_list = para_list->next;
    }

  return FALSE;
}

/* Given a paragraph and offset in that paragraph, find the
 * bounding rectangle for the character at the offset.
 */ 
void
char_bounds (GList *paragraphs, Paragraph *para, int index,
	     int width, PangoRectangle *rect)
{
  GList *para_list;
  
  int height = 0;
  int bytes_seen = 0;
  PangoDirection base_dir = pango_context_get_base_dir (context);
  
  para_list = paragraphs;
  while (para_list)
    {
      Paragraph *cur_para = para_list->data;
      
      if (cur_para == para)
	{
	  int line_height = 0;
	  
	  GSList *line_list = pango_layout_get_lines (para->layout);
	  while (line_list)
	    {
	      PangoLayoutLine *line = line_list->data;
	      PangoRectangle logical_rect;

	      pango_layout_line_get_extents (line_list->data, NULL, &logical_rect);

	      bytes_seen += line->length;
	      if (index < bytes_seen)
		{
		  int x0, x1;

		  pango_layout_line_index_to_x  (line, index, FALSE, &x0);
		  pango_layout_line_index_to_x  (line, index, TRUE, &x1);

		  if (x0 <= x1)
		    {
		      rect->x = x0 / 1000;
		      rect->width = (x1 / 1000) - rect->x; 
		    }
		  else
		    {
		      rect->x = x1 / 1000;
		      rect->width = (x0 / 1000) - rect->x; 
		    }
		  
		  rect->y = height + line_height / 1000;
		  rect->height = logical_rect.height / 1000;
		  
		  if (base_dir == PANGO_DIRECTION_RTL)
		    rect->x += width - logical_rect.width / 1000;
		  
		  return;
		}

	      line_height += logical_rect.height;
	      line_list = line_list->next;
	    }
	}
      
      height += cur_para->height;
      para_list = para_list->next;
    }
}

/* XOR a rectangle over a given character
 */
void
xor_char (GtkWidget *layout, GdkRectangle *clip_rect,
	  GList *paragraphs, Paragraph *para, int offset)
{
  static GdkGC *gc;
  PangoRectangle rect;		/* GdkRectangle in 1.2 is too limited
				 */
  if (!gc)
    {
      GdkGCValues values;
      values.foreground = layout->style->white.pixel ?
	layout->style->white : layout->style->black;
      values.function = GDK_XOR;
      gc = gdk_gc_new_with_values (GTK_LAYOUT (layout)->bin_window,
				   &values,
				   GDK_GC_FOREGROUND | GDK_GC_FUNCTION);
    }

  gdk_gc_set_clip_rectangle (gc, clip_rect);

  char_bounds (paragraphs, para, offset, layout->allocation.width,
	       &rect);

  rect.y -= GTK_LAYOUT (layout)->yoffset;

  if ((rect.y + rect.height >= 0) && (rect.y < layout->allocation.height))
    gdk_draw_rectangle (GTK_LAYOUT (layout)->bin_window, gc, TRUE,
			rect.x, rect.y, rect.width, rect.height);
}


/* Draw a paragraph on the screen by looping through the list
 * of lines, then for each line, looping through the list of
 * runs for that line and drawing them.
 */
void
expose_paragraph (Paragraph *para, GdkDrawable *drawable,
		  GdkGC *gc, int width, int x, int y)
{
  GSList *line_list;
  int line_height = 0;
  PangoRectangle logical_rect;
  PangoDirection base_dir = pango_context_get_base_dir (context);

  line_list = pango_layout_get_lines (para->layout);
  while (line_list)
    {
      PangoLayoutLine *line = line_list->data;
      line_list = line_list->next;

      pango_layout_line_get_extents (line, NULL, &logical_rect);

      if (base_dir == PANGO_DIRECTION_LTR)
	pango_x_render_layout_line (GDK_DISPLAY(), GDK_WINDOW_XWINDOW (drawable), GDK_GC_XGC (gc),
				    line, x, y + line_height / 1000);
      else
	pango_x_render_layout_line (GDK_DISPLAY(), GDK_WINDOW_XWINDOW (drawable), GDK_GC_XGC (gc),
				    line, x + width - logical_rect.width / 1000, y + line_height / 1000);
      
      line_height += logical_rect.height;
    }
}

/* Handle a size allocation by re-laying-out each paragraph to
 * the new width, setting the new size for the layout and
 * then queing a redraw
 */
void
size_allocate (GtkWidget *layout, GtkAllocation *allocation, GList *paragraphs)
{
  GList *tmp_list;
  int height = 0;

  tmp_list = paragraphs;
  while (tmp_list)
    {
      Paragraph *para = tmp_list->data;
      tmp_list = tmp_list->next;

      layout_paragraph (para, allocation->width);
      
      height += para->height;
    }

  gtk_layout_set_size (GTK_LAYOUT (layout), allocation->width, height);

  if (GTK_LAYOUT (layout)->yoffset + allocation->height > height)
    gtk_adjustment_set_value (GTK_LAYOUT (layout)->vadjustment,
			      height - allocation->height);
}

/* Handle a draw/expose by finding the paragraphs that intersect
 * the region and reexposing them.
 */
void
draw (GtkWidget *layout, GdkRectangle *area, GList *paragraphs)
{
  GList *tmp_list;
  int height = 0;
  
  gdk_draw_rectangle (GTK_LAYOUT (layout)->bin_window,
		      layout->style->base_gc[layout->state],
		      TRUE,
		      area->x, area->y, 
		      area->width, area->height);
  
  gdk_gc_set_clip_rectangle (layout->style->text_gc[layout->state], area);

  tmp_list = paragraphs;
  while (tmp_list &&
	 height < area->y + area->height + GTK_LAYOUT (layout)->yoffset)
    {
      Paragraph *para = tmp_list->data;
      tmp_list = tmp_list->next;
      
      if (height + para->height >= GTK_LAYOUT (layout)->yoffset + area->y)
	expose_paragraph (para,
			  GTK_LAYOUT (layout)->bin_window,
			  layout->style->text_gc[layout->state],
			  layout->allocation.width,
			  0, height - GTK_LAYOUT (layout)->yoffset);
      
      height += para->height;
    }

  gdk_gc_set_clip_rectangle (layout->style->text_gc[layout->state], NULL);

  if (highlight_para)
    xor_char (layout, area, paragraphs, highlight_para, highlight_offset);
}

gboolean
expose (GtkWidget *layout, GdkEventExpose *event, GList *paragraphs)
{
  if (event->window == GTK_LAYOUT (layout)->bin_window)
    draw (layout, &event->area, paragraphs);

  return TRUE;
}

void
button_press (GtkWidget *layout, GdkEventButton *event, GList *paragraphs)
{
  Paragraph *para = NULL;
  int offset;
  gchar *message;
  
  xy_to_cp (paragraphs, layout->allocation.width,
	    event->x, event->y + GTK_LAYOUT (layout)->yoffset,
	    &para, &offset);

  if (highlight_para)
    xor_char (layout, NULL, paragraphs, highlight_para, highlight_offset);

  highlight_para = para;
  highlight_offset = offset;
  
  if (para)
    {
      GUChar4 wc;

      unicode_get_utf8 (para->text + offset, &wc);
      message = g_strdup_printf ("Current char: U%04x", wc);
      
      xor_char (layout, NULL, paragraphs, highlight_para, highlight_offset);
    }
  else
    message = g_strdup_printf ("Current char:");

  gtk_label_set_text (GTK_LABEL (message_label), message);
  g_free (message);
}

static void
checkbutton_toggled (GtkWidget *widget, gpointer data)
{
  pango_context_set_base_dir (context, GTK_TOGGLE_BUTTON (widget)->active ? PANGO_DIRECTION_RTL : PANGO_DIRECTION_LTR);
  gtk_widget_queue_resize (layout);
}

static void
reload_font ()
{
  pango_context_set_font_description (context, &font_description);

  if (layout)
    gtk_widget_queue_resize (layout);
}

void
set_family (GtkWidget *entry, gpointer data)
{
  font_description.family_name = gtk_editable_get_chars (GTK_EDITABLE (entry), 0, -1);
  fill_styles_combo (styles_combo);
}

void
set_style (GtkWidget *entry, gpointer data)
{
  char *str = gtk_editable_get_chars (GTK_EDITABLE (entry), 0, -1);
  PangoFontDescription *tmp_desc;
  
  tmp_desc = pango_font_description_from_string (str);

  font_description.style = tmp_desc->style;
  font_description.variant = tmp_desc->variant;
  font_description.weight = tmp_desc->weight;
  font_description.stretch = tmp_desc->stretch;

  pango_font_description_free (tmp_desc);
  g_free (str);
  
  reload_font ();
}

void
font_size_changed (GtkAdjustment *adj)
{
  font_description.size = (int)(adj->value * 1000 + 0.5);
  reload_font();
}

static int
compare_font_descriptions (const PangoFontDescription *a, const PangoFontDescription *b)
{
  int val = strcmp (a->family_name, b->family_name);
  if (val != 0)
    return val;

  if (a->weight != b->weight)
    return a->weight - b->weight;

  if (a->style != b->style)
    return a->style - b->style;
  
  if (a->stretch != b->stretch)
    return a->stretch - b->stretch;

  if (a->variant != b->variant)
    return a->variant - b->variant;

  return 0;
}

static int
font_description_sort_func (const void *a, const void *b)
{
  return compare_font_descriptions (*(PangoFontDescription **)a, *(PangoFontDescription **)b);
}

typedef struct 
{
  PangoFontDescription **descs;
  int n_descs;
} FontDescInfo;

static void
free_info (FontDescInfo *info)
{
  pango_font_descriptions_free (info->descs, info->n_descs);
}

static void
fill_styles_combo (GtkWidget *combo)
{
  int i;
  GList *style_list = NULL;
  
  FontDescInfo *info = g_new (FontDescInfo, 1);
  pango_context_list_fonts (context, font_description.family_name, &info->descs, &info->n_descs);
  gtk_object_set_data_full (GTK_OBJECT (combo), "descs", info, (GtkDestroyNotify)free_info);

  qsort (info->descs, info->n_descs, sizeof(PangoFontDescription *), font_description_sort_func);

  for (i=0; i<info->n_descs; i++)
    {
      char *str;
	
      PangoFontDescription tmp_desc;

      tmp_desc = *info->descs[i];
      tmp_desc.family_name = NULL;
      tmp_desc.size = 0;
      
      str = pango_font_description_to_string (&tmp_desc);
      style_list = g_list_prepend (style_list, str);
    }

  style_list = g_list_reverse (style_list);

  gtk_combo_set_popdown_strings (GTK_COMBO (combo), style_list);
  g_list_foreach (style_list, (GFunc)g_free, NULL);
}

static GtkWidget *
make_styles_combo ()
{
  GtkWidget *combo;

  combo = gtk_combo_new ();
  gtk_combo_set_value_in_list (GTK_COMBO (combo), TRUE, FALSE);
  gtk_editable_set_editable (GTK_EDITABLE (GTK_COMBO (combo)->entry), FALSE);

  gtk_signal_connect (GTK_OBJECT (GTK_COMBO (combo)->entry), "changed",
		      GTK_SIGNAL_FUNC (set_style), NULL);

  styles_combo = combo;
  fill_styles_combo (combo);
  
  return combo;
}

int
cmp_strings (const void *a, const void *b)
{
  return strcmp (*(const char **)a, *(const char **)b);
}

GtkWidget *
make_families_menu ()
{
  GtkWidget *combo;
  gchar **families;
  int n_families;
  GList *family_list = NULL;
  int i;
  
  pango_context_list_families (context, &families, &n_families);
  qsort (families, n_families, sizeof(char *), cmp_strings);

  for (i=0; i<n_families; i++)
    family_list = g_list_prepend (family_list, families[i]);

  family_list = g_list_reverse (family_list);
  
  combo = gtk_combo_new ();
  gtk_combo_set_popdown_strings (GTK_COMBO (combo), family_list);
  gtk_combo_set_value_in_list (GTK_COMBO (combo), TRUE, FALSE);
  gtk_editable_set_editable (GTK_EDITABLE (GTK_COMBO (combo)->entry), FALSE);

  gtk_entry_set_text (GTK_ENTRY (GTK_COMBO (combo)->entry), font_description.family_name);

  gtk_signal_connect (GTK_OBJECT (GTK_COMBO (combo)->entry), "changed",
		      GTK_SIGNAL_FUNC (set_family), NULL);
  
  g_list_free (family_list);
  pango_font_map_free_families (families, n_families);

  return combo;
}


GtkWidget *
make_font_selector (void)
{
  GtkWidget *hbox;
  GtkWidget *util_hbox;
  GtkWidget *label;
  GtkWidget *option_menu;
  GtkWidget *spin_button;
  GtkAdjustment *adj;
  
  hbox = gtk_hbox_new (FALSE, 4);
  
  util_hbox = gtk_hbox_new (FALSE, 2);
  label = gtk_label_new ("Family:");
  gtk_box_pack_start (GTK_BOX (util_hbox), label, FALSE, FALSE, 0);
  option_menu = make_families_menu ();
  gtk_box_pack_start (GTK_BOX (util_hbox), option_menu, FALSE, FALSE, 0);
  
  gtk_box_pack_start (GTK_BOX (hbox), util_hbox, FALSE, FALSE, 0);

  util_hbox = gtk_hbox_new (FALSE, 2);
  label = gtk_label_new ("Style:");
  gtk_box_pack_start (GTK_BOX (util_hbox), label, FALSE, FALSE, 0);
  option_menu = make_styles_combo ();
  gtk_box_pack_start (GTK_BOX (util_hbox), option_menu, FALSE, FALSE, 0);
  
  gtk_box_pack_start (GTK_BOX (hbox), util_hbox, FALSE, FALSE, 0);

  util_hbox = gtk_hbox_new (FALSE, 2);
  label = gtk_label_new ("Size:");
  gtk_box_pack_start (GTK_BOX (util_hbox), label, FALSE, FALSE, 0);
  spin_button = gtk_spin_button_new (NULL, 1., 0);
  gtk_box_pack_start (GTK_BOX (util_hbox), spin_button, FALSE, FALSE, 0);

  gtk_box_pack_start (GTK_BOX (hbox), util_hbox, FALSE, FALSE, 0);

  adj = gtk_spin_button_get_adjustment (GTK_SPIN_BUTTON (spin_button));
  adj->value = font_description.size / 1000.;
  adj->lower = 0;
  adj->upper = 1024;
  adj->step_increment = 1;
  adj->page_size = 10;
  gtk_adjustment_changed (adj);
  gtk_adjustment_value_changed (adj);

  gtk_signal_connect (GTK_OBJECT (adj), "value_changed",
		      GTK_SIGNAL_FUNC (font_size_changed), NULL);
  
  return hbox;
}

int 
main (int argc, char **argv)
{
  char *text;
  GtkWidget *window;
  GtkWidget *scrollwin;
  GtkWidget *vbox, *hbox;
  GtkWidget *frame;
  GtkWidget *checkbutton;
  GList *paragraphs;

  gtk_init (&argc, &argv);
  
  if (argc != 2)
    {
      fprintf (stderr, "Usage: gscript-viewer FILE\n");
      exit(1);
    }

  /* Create the list of paragraphs from the supplied file
   */
  text = read_file (argv[1]);
  if (!text)
    exit(1);

  context = pango_x_get_context (GDK_DISPLAY());

  paragraphs = split_paragraphs (text);

  pango_context_set_lang (context, "en_US");
  pango_context_set_base_dir (context, PANGO_DIRECTION_LTR);

  font_description.family_name = g_strdup ("sans");
  font_description.style = PANGO_STYLE_NORMAL;
  font_description.variant = PANGO_VARIANT_NORMAL;
  font_description.weight = 500;
  font_description.stretch = PANGO_STRETCH_NORMAL;
  font_description.size = 16000;

  pango_context_set_font_description (context, &font_description);

  /* Create the user interface
   */
  window = gtk_window_new (GTK_WINDOW_TOPLEVEL);
  gtk_window_set_default_size (GTK_WINDOW (window), 400, 400);

  gtk_signal_connect (GTK_OBJECT (window), "destroy",
		      GTK_SIGNAL_FUNC (gtk_main_quit), NULL);

  vbox = gtk_vbox_new (FALSE, 4);
  gtk_container_add (GTK_CONTAINER (window), vbox);

  hbox = make_font_selector ();
  gtk_box_pack_start (GTK_BOX (vbox), hbox, FALSE, FALSE, 0);
  
  scrollwin = gtk_scrolled_window_new (NULL, NULL);
  gtk_scrolled_window_set_policy (GTK_SCROLLED_WINDOW (scrollwin),
				  GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
  
  gtk_box_pack_start (GTK_BOX (vbox), scrollwin, TRUE, TRUE, 0);
  
  layout = gtk_layout_new (NULL, NULL);
  gtk_widget_set_events (layout, GDK_BUTTON_PRESS_MASK);
  gtk_widget_set_app_paintable (layout, TRUE);

  gtk_signal_connect (GTK_OBJECT (layout), "size_allocate",
		      GTK_SIGNAL_FUNC (size_allocate), paragraphs);
  gtk_signal_connect (GTK_OBJECT (layout), "expose_event",
		      GTK_SIGNAL_FUNC (expose), paragraphs);
  gtk_signal_connect (GTK_OBJECT (layout), "draw",
		      GTK_SIGNAL_FUNC (draw), paragraphs);
  gtk_signal_connect (GTK_OBJECT (layout), "button_press_event",
		      GTK_SIGNAL_FUNC (button_press), paragraphs);

  gtk_container_add (GTK_CONTAINER (scrollwin), layout);

  frame = gtk_frame_new (NULL);
  gtk_frame_set_shadow_type (GTK_FRAME (frame), GTK_SHADOW_IN);
  gtk_box_pack_start (GTK_BOX (vbox), frame, FALSE, FALSE, 0);

  message_label = gtk_label_new ("Current char:");
  gtk_misc_set_padding (GTK_MISC (message_label), 1, 1);
  gtk_misc_set_alignment (GTK_MISC (message_label), 0.0, 0.5);
  gtk_container_add (GTK_CONTAINER (frame), message_label);

  checkbutton = gtk_check_button_new_with_label ("Use RTL global direction");
  gtk_signal_connect (GTK_OBJECT (checkbutton), "toggled",
		      GTK_SIGNAL_FUNC (checkbutton_toggled), NULL);
  gtk_box_pack_start (GTK_BOX (vbox), checkbutton, FALSE, FALSE, 0);

  gtk_widget_show_all (window);

  gtk_main ();
  
  return 0;
}
