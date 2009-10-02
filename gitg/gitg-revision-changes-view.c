/*
 * gitg-revision-changes-view.c
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

#include "gitg-revision-changes-view.h"
#include "gitg-diff-view.h"
#include "gitg-revision.h"
#include "gitg-runner.h"
#include "gitg-utils.h"

#define GITG_REVISION_CHANGES_VIEW_GET_PRIVATE(object)(G_TYPE_INSTANCE_GET_PRIVATE((object), GITG_TYPE_REVISION_CHANGES_VIEW, GitgRevisionChangesViewPrivate))

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

struct _GitgRevisionChangesViewPrivate
{
	GtkSourceView *diff;
	GtkTreeView *diff_files;
	GtkListStore *list_store_diff_files;
	
	GitgRunner *diff_runner;
	GitgRunner *diff_files_runner;
	
	GitgRepository *repository;
	GitgRevision *revision;
	GSList *cached_headers;
};

static void gitg_revision_changes_view_buildable_iface_init(GtkBuildableIface *iface);
static void on_header_added(GitgDiffView *view, GitgDiffIter *iter, GitgRevisionChangesView *self);
static void on_diff_files_selection_changed(GtkTreeSelection *selection, GitgRevisionChangesView *self);

G_DEFINE_TYPE_EXTENDED(GitgRevisionChangesView, gitg_revision_changes_view, GTK_TYPE_VBOX, 0,
	G_IMPLEMENT_INTERFACE(GTK_TYPE_BUILDABLE, gitg_revision_changes_view_buildable_iface_init));

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
revision_files_icon(GtkTreeViewColumn *column, GtkCellRenderer *renderer, GtkTreeModel *model, GtkTreeIter *iter, GitgRevisionChangesView *self)
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
revision_files_name(GtkTreeViewColumn *column, GtkCellRenderer *renderer, GtkTreeModel *model, GtkTreeIter *iter, GitgRevisionChangesView *self)
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
on_diff_files_button_press(GtkTreeView *treeview, GdkEventButton *event, GitgRevisionChangesView *view)
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
gitg_revision_changes_view_parser_finished(GtkBuildable *buildable, GtkBuilder *builder)
{
	if (parent_iface.parser_finished)
		parent_iface.parser_finished(buildable, builder);

	GitgRevisionChangesView *rvv = GITG_REVISION_CHANGES_VIEW(buildable);

	rvv->priv->diff = GTK_SOURCE_VIEW(gtk_builder_get_object(builder, "revision_diff"));
	rvv->priv->diff_files = GTK_TREE_VIEW(gtk_builder_get_object(builder, "tree_view_revision_files"));
	
	GtkTreeSelection *selection = gtk_tree_view_get_selection(rvv->priv->diff_files);
	gtk_tree_selection_set_mode(selection, GTK_SELECTION_MULTIPLE);
	g_signal_connect(selection, "changed", G_CALLBACK(on_diff_files_selection_changed), rvv);
	
	g_signal_connect(rvv->priv->diff_files, "button-press-event", G_CALLBACK(on_diff_files_button_press), rvv);
	
	rvv->priv->list_store_diff_files = gtk_list_store_new(1, diff_file_get_type());

	GtkTreeModel *filter = gtk_tree_model_filter_new(GTK_TREE_MODEL(rvv->priv->list_store_diff_files), NULL);
	gtk_tree_view_set_model(rvv->priv->diff_files, filter);
	
	gtk_tree_model_filter_set_visible_func(GTK_TREE_MODEL_FILTER(filter), diff_file_visible, NULL, NULL);
	
	gtk_tree_view_column_set_cell_data_func(GTK_TREE_VIEW_COLUMN(gtk_builder_get_object(builder, "revision_files_column_icon")),
											GTK_CELL_RENDERER(gtk_builder_get_object(builder, "revision_files_cell_renderer_icon")),
											(GtkTreeCellDataFunc)revision_files_icon,
											rvv,
											NULL);

	gtk_tree_view_column_set_cell_data_func(GTK_TREE_VIEW_COLUMN(gtk_builder_get_object(builder, "revision_files_column_name")),
											GTK_CELL_RENDERER(gtk_builder_get_object(builder, "revision_files_cell_renderer_name")),
											(GtkTreeCellDataFunc)revision_files_name,
											rvv,
											NULL);
	
	GtkSourceLanguageManager *manager = gtk_source_language_manager_get_default();
	GtkSourceLanguage *language = gtk_source_language_manager_get_language(manager, "gitgdiff");
	GtkSourceBuffer *buffer = gtk_source_buffer_new_with_language(language);
	g_object_unref(language);
	
	GtkSourceStyleSchemeManager *schememanager = gtk_source_style_scheme_manager_get_default();
	GtkSourceStyleScheme *scheme = gtk_source_style_scheme_manager_get_scheme(schememanager, "gitg");
	gtk_source_buffer_set_style_scheme(buffer, scheme);
	
	gitg_utils_set_monospace_font(GTK_WIDGET(rvv->priv->diff));
	gtk_text_view_set_buffer(GTK_TEXT_VIEW(rvv->priv->diff), GTK_TEXT_BUFFER(buffer));
	
	g_signal_connect(rvv->priv->diff, "header-added", G_CALLBACK(on_header_added), rvv);
}

static void
gitg_revision_changes_view_buildable_iface_init(GtkBuildableIface *iface)
{
	parent_iface = *iface;
	
	iface->parser_finished = gitg_revision_changes_view_parser_finished;
}

static void
free_cached_header(gpointer header)
{
	g_slice_free(CachedHeader, header);
}

static void
free_cached_headers(GitgRevisionChangesView *self)
{
	g_slist_foreach(self->priv->cached_headers, (GFunc)free_cached_header, NULL);
	g_slist_free(self->priv->cached_headers);
	self->priv->cached_headers = NULL;
}

static void
gitg_revision_changes_view_finalize(GObject *object)
{
	GitgRevisionChangesView *self = GITG_REVISION_CHANGES_VIEW(object);
	
	gitg_runner_cancel(self->priv->diff_runner);
	g_object_unref(self->priv->diff_runner);
	
	gitg_runner_cancel(self->priv->diff_files_runner);
	g_object_unref(self->priv->diff_files_runner);
	
	if (self->priv->repository)
	{
		g_object_unref(self->priv->repository);
	}
	
	free_cached_headers(self);

	G_OBJECT_CLASS(gitg_revision_changes_view_parent_class)->finalize(object);
}

static void
gitg_revision_changes_view_get_property(GObject *object, guint prop_id, GValue *value, GParamSpec *pspec)
{
	GitgRevisionChangesView *self = GITG_REVISION_CHANGES_VIEW(object);

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
gitg_revision_changes_view_set_property(GObject *object, guint prop_id, const GValue *value, GParamSpec *pspec)
{
	GitgRevisionChangesView *self = GITG_REVISION_CHANGES_VIEW(object);
	
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
gitg_revision_changes_view_class_init(GitgRevisionChangesViewClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS(klass);
	
	object_class->finalize = gitg_revision_changes_view_finalize;

	object_class->set_property = gitg_revision_changes_view_set_property;
	object_class->get_property = gitg_revision_changes_view_get_property;

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
			G_STRUCT_OFFSET (GitgRevisionChangesViewClass, parent_activated),
			NULL, NULL,
			g_cclosure_marshal_VOID__POINTER,
			G_TYPE_NONE,
			1, G_TYPE_POINTER);
	
	g_type_class_add_private(object_class, sizeof(GitgRevisionChangesViewPrivate));
}

static void
on_diff_files_begin_loading(GitgRunner *runner, GitgRevisionChangesView *self)
{
	GdkWindow *window = GTK_WIDGET(self->priv->diff_files)->window;
	if (window != NULL) {
		GdkCursor *cursor = gdk_cursor_new(GDK_WATCH);
		gdk_window_set_cursor(window, cursor);
		gdk_cursor_unref(cursor);
	}
}

static void
on_diff_files_end_loading(GitgRunner *runner, gboolean cancelled, GitgRevisionChangesView *self)
{
	GdkWindow *window = GTK_WIDGET(self->priv->diff_files)->window;
	if (window != NULL) {
		gdk_window_set_cursor(window, NULL);
	}
}

static gboolean
match_indices(DiffFile *f, gchar const *from, gchar const *to)
{
	return g_str_has_prefix(f->index_from, from) && g_str_has_prefix(f->index_to, to);
}

static void
visible_from_cached_headers(GitgRevisionChangesView *view, DiffFile *f)
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
add_diff_file(GitgRevisionChangesView *view, DiffFile *f)
{
	GtkTreeIter iter;
	gtk_list_store_append(view->priv->list_store_diff_files, &iter);
	
	/* see if it is in the cached headers */
	visible_from_cached_headers(view, f);

	gtk_list_store_set(view->priv->list_store_diff_files, &iter, 0, f, -1);
}

static void
on_diff_files_update(GitgRunner *runner, gchar **buffer, GitgRevisionChangesView *self)
{
	gchar **line;
	
	while (*(line = buffer++))
	{
		if (**line == '\0')
			continue;
		
		// Count parents
		gint parents = 0;
		gchar *ptr = *line;
		
		while (*(ptr++) == ':')
			++parents;
		
		gint numparts = 3 + 2 * parents;
		gchar **parts = g_strsplit(ptr, " ", numparts);
		
		if (g_strv_length(parts) == numparts)
		{
			gchar **files = g_strsplit(parts[numparts - 1], "\t", -1);

			DiffFile *f = diff_file_new(parts[parents + 1], parts[numparts - 2], files[0], files[1]);
			
			add_diff_file(self, f);
			diff_file_unref(f);

			g_strfreev(files);
		}

		g_strfreev(parts);
	}
}

static void
on_diff_begin_loading(GitgRunner *runner, GitgRevisionChangesView *self)
{
	GdkWindow *window = GTK_WIDGET(self->priv->diff)->window;
	if (window != NULL) {
		GdkCursor *cursor = gdk_cursor_new(GDK_WATCH);
		gdk_window_set_cursor(window, cursor);
		gdk_cursor_unref(cursor);
	}
}

static void
on_diff_end_loading(GitgRunner *runner, gboolean cancelled, GitgRevisionChangesView *self)
{
	GdkWindow *window = GTK_WIDGET(self->priv->diff)->window;
	if (window != NULL)
	{
		gdk_window_set_cursor(window, NULL);
	}
	
	if (cancelled)
		return;

	if (!self->priv->revision)
		return;

	gchar sign = gitg_revision_get_sign(self->priv->revision);
	
	if (sign == 't' || sign == 'u')
	{
		gchar *head = gitg_repository_parse_head(self->priv->repository);
		const gchar *cached = NULL;
		
		if (sign == 't')
			cached = "--cached";

		gitg_repository_run_commandv(self->priv->repository, self->priv->diff_files_runner, NULL,
									"diff-index", "--raw", "-M", "--abbrev=40", head, cached, NULL);
		g_free(head);
	}
	else
	{
		gchar *sha = gitg_revision_get_sha1(self->priv->revision);
		gitg_repository_run_commandv(self->priv->repository, self->priv->diff_files_runner, NULL,
								 "show", "--encoding=UTF-8", "--raw", "-M", "--pretty=format:", "--abbrev=40", sha, NULL);
		g_free(sha);
	}
}

static void
on_diff_update(GitgRunner *runner, gchar **buffer, GitgRevisionChangesView *self)
{
	gchar *line;
	GtkTextBuffer *buf = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->priv->diff));
	GtkTextIter iter;
	
	gtk_text_buffer_get_end_iter(buf, &iter);
	
	while ((line = *buffer++))
	{
		gtk_text_buffer_insert(buf, &iter, line, -1);
		gtk_text_buffer_insert(buf, &iter, "\n", -1);
	}
}

static void
gitg_revision_changes_view_init(GitgRevisionChangesView *self)
{
	self->priv = GITG_REVISION_CHANGES_VIEW_GET_PRIVATE(self);
	
	self->priv->diff_runner = gitg_runner_new(2000);
	
	g_signal_connect(self->priv->diff_runner, "begin-loading", G_CALLBACK(on_diff_begin_loading), self);
	g_signal_connect(self->priv->diff_runner, "update", G_CALLBACK(on_diff_update), self);
	g_signal_connect(self->priv->diff_runner, "end-loading", G_CALLBACK(on_diff_end_loading), self);
	
	self->priv->diff_files_runner = gitg_runner_new(2000);
	
	g_signal_connect(self->priv->diff_files_runner, "begin-loading", G_CALLBACK(on_diff_files_begin_loading), self);
	g_signal_connect(self->priv->diff_files_runner, "update", G_CALLBACK(on_diff_files_update), self);
	g_signal_connect(self->priv->diff_files_runner, "end-loading", G_CALLBACK(on_diff_files_end_loading), self);
}

#define HASH_KEY "GitgRevisionChangesViewHashKey"

static gboolean
on_parent_clicked(GtkWidget *ev, GdkEventButton *event, gpointer userdata)
{
	if (event->button != 1)
		return FALSE;
	
	GitgRevisionChangesView *rvv = GITG_REVISION_CHANGES_VIEW(userdata);
	
	gchar *hash = (gchar *)g_object_get_data(G_OBJECT(ev), HASH_KEY);
	g_signal_emit(rvv, signals[PARENT_ACTIVATED], 0, hash);

	return FALSE;
}

static GtkWidget *
make_parent_label(GitgRevisionChangesView *self, gchar const *hash)
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
update_diff(GitgRevisionChangesView *self, GitgRepository *repository)
{	
	GtkTreeSelection *selection;
	
	// First cancel a possibly still running diff
	gitg_runner_cancel(self->priv->diff_runner);
	gitg_runner_cancel(self->priv->diff_files_runner);
	
	free_cached_headers(self);
	
	// Clear the buffer
	GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(self->priv->diff));
	gtk_text_buffer_set_text(buffer, "", 0);
	
	selection = gtk_tree_view_get_selection(self->priv->diff_files);
	g_signal_handlers_block_by_func(selection,
	                                G_CALLBACK(on_diff_files_selection_changed),
	                                self);

	gtk_list_store_clear(self->priv->list_store_diff_files);
	
	g_signal_handlers_unblock_by_func(selection,
	                                  G_CALLBACK(on_diff_files_selection_changed),
	                                  self);
	
	if (!self->priv->revision)
		return;

	gchar sign = gitg_revision_get_sign(self->priv->revision);
	
	switch (sign)
	{
		case 't':
			gitg_repository_run_commandv(self->priv->repository, self->priv->diff_runner, NULL,
										"diff", "--cached", "-M", "--pretty=format:%s%n%n%b",
										"--encoding=UTF-8", NULL);
		break;
		case 'u':
			gitg_repository_run_commandv(self->priv->repository, self->priv->diff_runner, NULL,
										"diff", "-M", "--pretty=format:%s%n%n%b",
										"--encoding=UTF-8", NULL);
		break;
		default:
		{
			gchar *hash = gitg_revision_get_sha1(self->priv->revision);
			gitg_repository_run_commandv(self->priv->repository, self->priv->diff_runner, NULL,
										 "show", "-M", "--pretty=format:%s%n%n%b", 
										 "--encoding=UTF-8", hash, NULL);

			g_free(hash);
		}
		break;
	}
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
gitg_revision_changes_view_update(GitgRevisionChangesView *self, GitgRepository *repository, GitgRevision *revision)
{
	GtkClipboard *cb;

	g_return_if_fail(GITG_IS_REVISION_CHANGES_VIEW(self));
	
	// Update diff
	self->priv->revision = revision;
	update_diff(self, repository);
}

void 
gitg_revision_changes_view_set_repository(GitgRevisionChangesView *view, GitgRepository *repository)
{
	g_return_if_fail(GITG_IS_REVISION_CHANGES_VIEW(view));
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

static gboolean
find_diff_file(GitgRevisionChangesView *view, GitgDiffIter *iter, GtkTreeIter *it, DiffFile **f)
{
	gchar *from;
	gchar *to;
	
	if (!gitg_diff_iter_get_index(iter, &from, &to))
		return FALSE;
	
	GtkTreeModel *model = GTK_TREE_MODEL(view->priv->list_store_diff_files);
	
	if (!gtk_tree_model_get_iter_first(model, it))
		return FALSE;
	
	do
	{
		gtk_tree_model_get(model, it, 0, f, -1);
		
		if (match_indices(*f, from, to))
			return TRUE;
		
		diff_file_unref(*f);
	} while (gtk_tree_model_iter_next(model, it));
	
	return FALSE;
}

static void 
on_header_added(GitgDiffView *view, GitgDiffIter *iter, GitgRevisionChangesView *self)
{
	GtkTreeIter it;
	DiffFile *f;
	
	gchar *from = NULL, *to = NULL;
	gitg_diff_iter_get_index(iter, &from, &to);
	
	if (find_diff_file(self, iter, &it, &f))
	{
		if (!f->visible)
		{
			f->visible = TRUE;
			f->iter = *iter;

			diff_file_unref(f);
			
			GtkTreeModel *model = GTK_TREE_MODEL(self->priv->list_store_diff_files);
			GtkTreePath *path = gtk_tree_model_get_path(model, &it);
			
			gtk_tree_model_row_changed(model, path, &it);
			gtk_tree_path_free(path);
		}
	}
	else
	{
		/* Insert in cached headers */
		CachedHeader *header = g_slice_new(CachedHeader);
		header->iter = *iter;

		self->priv->cached_headers = g_slist_prepend(self->priv->cached_headers, header);
	}
}

typedef struct
{
	gint numselected;
	GtkTreeSelection *selection;
} ForeachSelectionData;

static gboolean
foreach_selection_changed(GtkTreeModel *model, GtkTreePath *path, GtkTreeIter *iter, ForeachSelectionData *data)
{
	gboolean visible = data->numselected == 0 || gtk_tree_selection_path_is_selected(data->selection, path);
	
	DiffFile *f = NULL;
	gtk_tree_model_get(model, iter, 0, &f, -1);
	
	if (f->visible)
	{
		gitg_diff_iter_set_visible(&f->iter, visible);
	}

	diff_file_unref(f);
	return FALSE;
}

static void 
on_diff_files_selection_changed(GtkTreeSelection *selection, GitgRevisionChangesView *self)
{
	ForeachSelectionData data = {gtk_tree_selection_count_selected_rows(selection), selection};
	gtk_tree_model_foreach(gtk_tree_view_get_model(self->priv->diff_files), (GtkTreeModelForeachFunc)foreach_selection_changed, &data);
}
