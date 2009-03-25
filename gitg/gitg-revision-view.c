/*
 * gitg-revision-view.c
 * This file is part of gitg - git repository viewer
 *
 * Copyright (C) 2009 - Jesse van den Kieboom
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, 
 * Boston, MA 02111-1307, USA.
 */

#include <gtksourceview/gtksourceview.h>
#include <gtksourceview/gtksourcelanguagemanager.h>
#include <gtksourceview/gtksourcestyleschememanager.h>
#include <string.h>

#include "gitg-revision-view.h"
#include "gitg-diff-view.h"
#include "gitg-revision.h"
#include "gitg-runner.h"
#include "gitg-utils.h"

#define GITG_REVISION_VIEW_GET_PRIVATE(object)(G_TYPE_INSTANCE_GET_PRIVATE((object), GITG_TYPE_REVISION_VIEW, GitgRevisionViewPrivate))

/* Properties */
enum
{
	PROP_0,
	PROP_REPOSITORY
};

/* Signals */
enum
{
	PARENT_ACTIVATED,
	NUM_SIGNALS
};

static guint signals[NUM_SIGNALS];

typedef struct
{
	GitgDiffIter iter;
} CachedHeader;

struct _GitgRevisionViewPrivate
{
	GtkLabel *sha;
	GtkLabel *author;
	GtkLabel *date;
	GtkLabel *subject;
	GtkTable *parents;
	GtkTextView *log;
	
	GitgRepository *repository;
	GitgRevision *revision;
	GSList *cached_headers;
};

static void gitg_revision_view_buildable_iface_init(GtkBuildableIface *iface);
static void on_header_added(GitgDiffView *view, GitgDiffIter *iter, GitgRevisionView *self);
static void on_diff_files_selection_changed(GtkTreeSelection *selection, GitgRevisionView *self);

G_DEFINE_TYPE_EXTENDED(GitgRevisionView, gitg_revision_view, GTK_TYPE_VBOX, 0,
	G_IMPLEMENT_INTERFACE(GTK_TYPE_BUILDABLE, gitg_revision_view_buildable_iface_init));

static GtkBuildableIface parent_iface;

typedef enum
{
	DIFF_FILE_STATUS_NONE,
	DIFF_FILE_STATUS_NEW,
	DIFF_FILE_STATUS_MODIFIED,
	DIFF_FILE_STATUS_DELETED
} DiffFileStatus;

typedef struct
{
	gint refcount;

	gchar index_from[HASH_SHA_SIZE + 1];
	gchar index_to[HASH_SHA_SIZE + 1];
	DiffFileStatus status;
	gchar *filename;

	gboolean visible;
	GitgDiffIter iter;
} DiffFile;

static DiffFile *
diff_file_new(gchar const *from, gchar *to, gchar const *status, gchar const *filename)
{
	DiffFile *f = g_slice_new(DiffFile);
	
	strncpy(f->index_from, from, HASH_SHA_SIZE);
	strncpy(f->index_to, to, HASH_SHA_SIZE);
	
	f->index_from[HASH_SHA_SIZE] = '\0';
	f->index_to[HASH_SHA_SIZE] = '\0';
	f->visible = FALSE;
	
	DiffFileStatus st;
	
	switch (*status)
	{
		case 'A':
			st = DIFF_FILE_STATUS_NEW;
		break;
		case 'D':
			st = DIFF_FILE_STATUS_DELETED;
		break;
		default:
			st = DIFF_FILE_STATUS_MODIFIED;
		break;
	}
	
	f->status = st;
	f->filename = g_strdup(filename);
	f->refcount = 1;

	return f;
}

static DiffFile *
diff_file_copy(DiffFile *f)
{
	g_atomic_int_inc(&f->refcount);
	return f;
}

static void
diff_file_unref(DiffFile *f)
{
	if (!g_atomic_int_dec_and_test(&f->refcount))
		return;

	g_free(f->filename);
	g_slice_free(DiffFile, f);
}

static GType
diff_file_get_type()
{
	static GType gtype = 0;
	
	if (!G_UNLIKELY(gtype))
		gtype = g_boxed_type_register_static("DiffFile", (GBoxedCopyFunc)diff_file_copy, (GBoxedFreeFunc)diff_file_unref);
	
	return gtype;
}

static void
update_markup(GObject *object)
{
	GtkLabel *label = GTK_LABEL(object);
	gchar const *text = gtk_label_get_text(label);
	
	gchar *newtext = g_strconcat("<span weight='bold' foreground='#777'>", text, "</span>", NULL);

	gtk_label_set_markup(label, newtext);
	g_free(newtext);
}

static void
revision_files_icon(GtkTreeViewColumn *column, GtkCellRenderer *renderer, GtkTreeModel *model, GtkTreeIter *iter, GitgRevisionView *self)
{
	DiffFile *f;
	gtk_tree_model_get(model, iter, 0, &f, -1);
	
	gchar const *id = NULL;
	
	switch (f->status)
	{
		case DIFF_FILE_STATUS_NEW:
			id = GTK_STOCK_NEW;
		break;
		case DIFF_FILE_STATUS_MODIFIED:
			id = GTK_STOCK_EDIT;
		break;
		case DIFF_FILE_STATUS_DELETED:
			id = GTK_STOCK_DELETE;
		break;
		default:
		break;
	}
	
	g_object_set(G_OBJECT(renderer), "stock-id", id, NULL);
	diff_file_unref(f);
}

static void
revision_files_name(GtkTreeViewColumn *column, GtkCellRenderer *renderer, GtkTreeModel *model, GtkTreeIter *iter, GitgRevisionView *self)
{
	DiffFile *f;
	gtk_tree_model_get(model, iter, 0, &f, -1);
	
	g_object_set(G_OBJECT(renderer), "text", f->filename, NULL);
	
	diff_file_unref(f);
}

static gboolean
diff_file_visible(GtkTreeModel *model, GtkTreeIter *iter, gpointer data)
{
	DiffFile *f;
	gtk_tree_model_get(model, iter, 0, &f, -1);
	
	if (!f)
		return FALSE;

	gboolean ret = f->visible;
	diff_file_unref(f);
	
	return ret;
}

static gboolean
on_diff_files_button_press(GtkTreeView *treeview, GdkEventButton *event, GitgRevisionView *view)
{
	if (event->button != 1)
		return FALSE;
	
	if (event->window != gtk_tree_view_get_bin_window(treeview))
		return FALSE;

	GtkTreePath *path;
	if (!gtk_tree_view_get_path_at_pos(treeview, event->x, event->y, &path, NULL, NULL, NULL))
		return FALSE;
	
	GtkTreeSelection *selection = gtk_tree_view_get_selection(treeview);
	gboolean ret = FALSE;
	
	if (gtk_tree_selection_path_is_selected(selection, path) && gtk_tree_selection_count_selected_rows(selection) == 1)
	{
		/* deselect */
		gtk_tree_selection_unselect_path(selection, path);
		ret = TRUE;
	}
	
	gtk_tree_path_free(path);
	return ret;
}

static void
gitg_revision_view_parser_finished(GtkBuildable *buildable, GtkBuilder *builder)
{
	if (parent_iface.parser_finished)
		parent_iface.parser_finished(buildable, builder);

	GitgRevisionView *rvv = GITG_REVISION_VIEW(buildable);

	rvv->priv->sha = GTK_LABEL(gtk_builder_get_object(builder, "label_sha"));
	rvv->priv->author = GTK_LABEL(gtk_builder_get_object(builder, "label_author"));
	rvv->priv->date = GTK_LABEL(gtk_builder_get_object(builder, "label_date"));
	rvv->priv->subject = GTK_LABEL(gtk_builder_get_object(builder, "label_subject"));
	rvv->priv->parents = GTK_TABLE(gtk_builder_get_object(builder, "table_parents"));
	rvv->priv->log = GTK_TEXT_VIEW(gtk_builder_get_object(builder, "text_view_log"));
	
	gchar const *lbls[] = {
		"label_subject_lbl",
		"label_author_lbl",
		"label_sha_lbl",
		"label_date_lbl",
		"label_parent_lbl"
	};
	
	int i;
	for (i = 0; i < sizeof(lbls) / sizeof(gchar *); ++i)
		update_markup(gtk_builder_get_object(builder, lbls[i]));
}

static void
gitg_revision_view_buildable_iface_init(GtkBuildableIface *iface)
{
	parent_iface = *iface;
	
	iface->parser_finished = gitg_revision_view_parser_finished;
}

static void
free_cached_header(gpointer header)
{
	g_slice_free(CachedHeader, header);
}

static void
free_cached_headers(GitgRevisionView *self)
{
	g_slist_foreach(self->priv->cached_headers, (GFunc)free_cached_header, NULL);
	g_slist_free(self->priv->cached_headers);
	self->priv->cached_headers = NULL;
}

static void
gitg_revision_view_finalize(GObject *object)
{
	GitgRevisionView *self = GITG_REVISION_VIEW(object);
	
	g_object_unref(self->priv->repository);
	
	free_cached_headers(self);

	G_OBJECT_CLASS(gitg_revision_view_parent_class)->finalize(object);
}

static void
gitg_revision_view_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	GitgRevisionView *self = GITG_REVISION_VIEW(object);

	switch (prop_id)
	{
		case PROP_REPOSITORY:
			g_value_set_object(value, self->priv->repository);
		break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}

static void
gitg_revision_view_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	GitgRevisionView *self = GITG_REVISION_VIEW(object);
	
	switch (prop_id)
	{
		case PROP_REPOSITORY:
		{
			if (self->priv->repository)
				g_object_unref(self->priv->repository);
				
			self->priv->repository = g_value_dup_object(value);
		}
		break;
		default:
			G_OBJECT_WARN_INVALID_PROPERTY_ID(object, prop_id, pspec);
		break;
	}
}


static void
gitg_revision_view_class_init(GitgRevisionViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	
	object_class->finalize = gitg_revision_view_finalize;

	object_class->set_property = gitg_revision_view_set_property;
	object_class->get_property = gitg_revision_view_get_property;

	g_object_class_install_property(object_class, PROP_REPOSITORY,
					 g_param_spec_object("repository",
							      "REPOSITORY",
							      "Repository",
							      GITG_TYPE_REPOSITORY,
							      G_PARAM_READWRITE));

	signals[PARENT_ACTIVATED] =
		g_signal_new("parent-activated",
			G_OBJECT_CLASS_TYPE (object_class),
			G_SIGNAL_RUN_LAST,
			G_STRUCT_OFFSET (GitgRevisionViewClass, parent_activated),
			NULL, NULL,
			g_cclosure_marshal_VOID__POINTER,
			G_TYPE_NONE,
			1, G_TYPE_POINTER);
	
	g_type_class_add_private(object_class, sizeof(GitgRevisionViewPrivate));
}

static gboolean
match_indices(DiffFile *f, gchar const *from, gchar const *to)
{
	return g_str_has_prefix(f->index_from, from) && g_str_has_prefix(f->index_to, to);
}

static void
visible_from_cached_headers(GitgRevisionView *view, DiffFile *f)
{
	GSList *item;
	
	for (item = view->priv->cached_headers; item; item = g_slist_next(item))
	{
		CachedHeader *header = (CachedHeader *)item->data;
		gchar *from;
		gchar *to;

		if (gitg_diff_iter_get_index(&header->iter, &from, &to) && match_indices(f, from, to))
		{
			f->visible = TRUE;
			f->iter = header->iter;
			return;
		}
	}
}

static void
gitg_revision_view_init(GitgRevisionView *self)
{
	self->priv = GITG_REVISION_VIEW_GET_PRIVATE(self);
	
}

#define HASH_KEY "GitgRevisionViewHashKey"

static gboolean
on_parent_clicked(GtkWidget *ev, GdkEventButton *event, gpointer userdata)
{
	if (event->button != 1)
		return FALSE;
	
	GitgRevisionView *rvv = GITG_REVISION_VIEW(userdata);
	
	gchar *hash = (gchar *)g_object_get_data(G_OBJECT(ev), HASH_KEY);
	g_signal_emit(rvv, signals[PARENT_ACTIVATED], 0, hash);

	return FALSE;
}

static GtkWidget *
make_parent_label(GitgRevisionView *self, gchar const *hash)
{
	GtkWidget *ev = gtk_event_box_new();
	GtkWidget *lbl = gtk_label_new(NULL);
	
	gchar *markup = g_strconcat("<span underline='single' foreground='#00f'>", hash, "</span>", NULL);
	gtk_label_set_markup(GTK_LABEL(lbl), markup);
	g_free(markup);

	gtk_misc_set_alignment(GTK_MISC(lbl), 0.0, 0.5);
	gtk_container_add(GTK_CONTAINER(ev), lbl);
	
	gtk_widget_show(ev);
	gtk_widget_show(lbl);
	
	g_object_set_data_full(G_OBJECT(ev), HASH_KEY, (gpointer)gitg_utils_sha1_to_hash_new(hash), (GDestroyNotify)g_free);
	g_signal_connect(ev, "button-release-event", G_CALLBACK(on_parent_clicked), self);

	return ev;
}

static void
update_parents(GitgRevisionView *self, GitgRevision *revision)
{
	GList *children = gtk_container_get_children(GTK_CONTAINER(self->priv->parents));
	GList *item;
	
	for (item = children; item; item = item->next)
		gtk_container_remove(GTK_CONTAINER(self->priv->parents), GTK_WIDGET(item->data));
	
	g_list_free(children);
	
	if (!revision)
		return;
	
	gchar **parents = gitg_revision_get_parents(revision);
	gint num = g_strv_length(parents);
	gint i;
	
	gtk_table_resize(self->priv->parents, num ? num : num + 1, 2);
	GdkCursor *cursor = gdk_cursor_new(GDK_HAND1);
	Hash hash;
	
	for (i = 0; i < num; ++i)
	{
		GtkWidget *widget = make_parent_label(self, parents[i]);
		gtk_table_attach(self->priv->parents, widget, 0, 1, i, i + 1, GTK_FILL | GTK_SHRINK, GTK_FILL | GTK_SHRINK, 0, 0);
		
		gtk_widget_realize(widget);
		gdk_window_set_cursor(widget->window, cursor);
		
		/* find subject */
		gitg_utils_sha1_to_hash(parents[i], hash);
		
		GitgRevision *revision = gitg_repository_lookup(self->priv->repository, hash);
		
		if (revision)
		{
			GtkWidget *subject = gtk_label_new(NULL);

			gchar *escaped = g_markup_escape_text(gitg_revision_get_subject(revision), -1);
			gchar *text = g_strdup_printf("(<i>%s</i>)", escaped);
			
			gtk_label_set_markup(GTK_LABEL(subject), text);
			
			g_free(escaped);
			g_free(text);
			
			gtk_widget_show(subject);

			gtk_misc_set_alignment(GTK_MISC(subject), 0.0, 0.5);
			gtk_label_set_ellipsize(GTK_LABEL(subject), PANGO_ELLIPSIZE_MIDDLE);
			gtk_label_set_single_line_mode(GTK_LABEL(subject), TRUE);
			
			gtk_table_attach(self->priv->parents, subject, 1, 2, i, i + 1, GTK_FILL | GTK_EXPAND, GTK_FILL | GTK_SHRINK, 0, 0);
		}
	}

	gdk_cursor_unref(cursor);	
	g_strfreev(parents);	
}

static gchar *
format_date(GitgRevision *revision)
{
	guint64 timestamp = gitg_revision_get_timestamp(revision);
	time_t t = timestamp;
	
	char *ptr = ctime(&t);
	
	// Remove newline?
	ptr[strlen(ptr) - 1] = '\0';
	
	return ptr;
}

void
gitg_revision_view_update(GitgRevisionView *self, GitgRepository *repository, GitgRevision *revision)
{
	GtkClipboard *cb;
	GtkTextBuffer *tb;

	g_return_if_fail(GITG_IS_REVISION_VIEW(self));

	tb = gtk_text_view_get_buffer(self->priv->log);

	// Update labels
	if (revision)
	{
		gtk_label_set_text(self->priv->author, gitg_revision_get_author(revision));

		gchar *s = g_markup_escape_text(gitg_revision_get_subject(revision), -1);
		gtk_text_buffer_set_text(tb, s, -1);
		gchar *subject = g_strconcat("<b>", s, "</b>", NULL);
		g_free(s);

		gtk_label_set_markup(self->priv->subject, subject);
		g_free(subject);

		gtk_label_set_text(self->priv->date, format_date(revision));
	
		gchar *sha = gitg_revision_get_sha1(revision);
		gtk_label_set_text(self->priv->sha, sha);

		cb = gtk_clipboard_get(GDK_SELECTION_PRIMARY);
		gtk_clipboard_set_text(cb, sha, -1);

		g_free(sha);
	}
	else
	{
		gtk_label_set_text(self->priv->author, "");
		gtk_label_set_text(self->priv->subject, "");
		gtk_text_buffer_set_text(tb, "", -1);
		gtk_label_set_text(self->priv->date, "");
		gtk_label_set_text(self->priv->sha, "");
	}
	
	// Update parents
	update_parents(self, revision);
}

void 
gitg_revision_view_set_repository(GitgRevisionView *view, GitgRepository *repository)
{
	g_return_if_fail(GITG_IS_REVISION_VIEW(view));
	g_return_if_fail(repository == NULL || GITG_IS_REPOSITORY(repository));

	if (view->priv->repository)
	{
		g_object_unref(view->priv->repository);
		view->priv->repository = NULL;
	}
	
	if (repository)
		view->priv->repository = g_object_ref(repository);
	
	g_object_notify(G_OBJECT(view), "repository");
}
