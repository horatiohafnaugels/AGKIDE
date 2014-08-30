/*
 *      project.c - this file is part of Geany, a fast and lightweight IDE
 *
 *      Copyright 2007-2012 Enrico Tröger <enrico(dot)troeger(at)uvena(dot)de>
 *      Copyright 2007-2012 Nick Treleaven <nick(dot)treleaven(at)btinternet(dot)com>
 *
 *      This program is free software; you can redistribute it and/or modify
 *      it under the terms of the GNU General Public License as published by
 *      the Free Software Foundation; either version 2 of the License, or
 *      (at your option) any later version.
 *
 *      This program is distributed in the hope that it will be useful,
 *      but WITHOUT ANY WARRANTY; without even the implied warranty of
 *      MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *      GNU General Public License for more details.
 *
 *      You should have received a copy of the GNU General Public License along
 *      with this program; if not, write to the Free Software Foundation, Inc.,
 *      51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

/** @file project.h
 * Project Management.
 */

#include "geany.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>

#include "project.h"

#include "dialogs.h"
#include "support.h"
#include "utils.h"
#include "ui_utils.h"
#include "document.h"
#include "msgwindow.h"
#include "main.h"
#include "keyfile.h"
#include "win32.h"
#include "build.h"
#include "editor.h"
#include "stash.h"
#include "sidebar.h"
#include "filetypes.h"
#include "templates.h"

GPtrArray *projects_array = NULL;

ProjectPrefs project_prefs = { NULL, FALSE, FALSE };


static GSList *stash_groups = NULL;

static struct
{
	gchar *project_file_path; /* in UTF-8 */
} local_prefs = { NULL };

static gboolean entries_modified;

/* simple struct to keep references to the elements of the properties dialog */
typedef struct _PropertyDialogElements
{
	GtkWidget *dialog;
	GtkWidget *notebook;
	GtkWidget *name;
	GtkWidget *description;
	GtkWidget *file_name;
	GtkWidget *base_path;
	GtkWidget *patterns;
	BuildTableData build_properties;
	gint build_page_num;
} PropertyDialogElements;


static gboolean update_config(const PropertyDialogElements *e, gboolean new_project);
static void on_file_save_button_clicked(GtkButton *button, PropertyDialogElements *e);
static gboolean load_config(const gchar *filename);
static gboolean write_config(GeanyProject *project, gboolean emit_signal);
static void on_name_entry_changed(GtkEditable *editable, PropertyDialogElements *e);
static void on_entries_changed(GtkEditable *editable, PropertyDialogElements *e);
static void on_radio_long_line_custom_toggled(GtkToggleButton *radio, GtkWidget *spin_long_line);
static void apply_editor_prefs(void);
static void init_stash_prefs(void);


#define SHOW_ERR(args) dialogs_show_msgbox(GTK_MESSAGE_ERROR, args)
#define SHOW_ERR1(args, more) dialogs_show_msgbox(GTK_MESSAGE_ERROR, args, more)
#define MAX_NAME_LEN 50
/* "projects" is part of the default project base path so be careful when translating
 * please avoid special characters and spaces, look at the source for details or ask Frank */
#define PROJECT_DIR _("projects")


/* TODO: this should be ported to Glade like the project preferences dialog,
 * then we can get rid of the PropertyDialogElements struct altogether as
 * widgets pointers can be accessed through ui_lookup_widget(). */
void project_new(void)
{
	GtkWidget *vbox;
	GtkWidget *table;
	GtkWidget *image;
	GtkWidget *button;
	GtkWidget *bbox;
	GtkWidget *label;
	PropertyDialogElements *e;

	e = g_new0(PropertyDialogElements, 1);
	e->dialog = gtk_dialog_new_with_buttons(_("New Project"), GTK_WINDOW(main_widgets.window),
										 GTK_DIALOG_DESTROY_WITH_PARENT,
										 GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL, NULL);

	gtk_widget_set_name(e->dialog, "GeanyDialogProject");
	bbox = gtk_hbox_new(FALSE, 0);
	button = gtk_button_new();
	gtk_widget_set_can_default(button, TRUE);
	gtk_window_set_default(GTK_WINDOW(e->dialog), button);
	image = gtk_image_new_from_stock(GTK_STOCK_NEW, GTK_ICON_SIZE_BUTTON);
	label = gtk_label_new_with_mnemonic(_("C_reate"));
	gtk_box_pack_start(GTK_BOX(bbox), image, FALSE, FALSE, 3);
	gtk_box_pack_start(GTK_BOX(bbox), label, FALSE, FALSE, 3);
	gtk_container_add(GTK_CONTAINER(button), bbox);
	gtk_dialog_add_action_widget(GTK_DIALOG(e->dialog), button, GTK_RESPONSE_OK);
	gtk_window_set_default_size(GTK_WINDOW(e->dialog),500,100);

	vbox = ui_dialog_vbox_new(GTK_DIALOG(e->dialog));

	entries_modified = FALSE;

	table = gtk_table_new(2, 2, FALSE);
	gtk_table_set_row_spacings(GTK_TABLE(table), 5);
	gtk_table_set_col_spacings(GTK_TABLE(table), 10);

	label = gtk_label_new(_("Name:"));
	gtk_misc_set_alignment(GTK_MISC(label), 1, 0);

	e->name = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(e->name), TRUE);
	ui_entry_add_clear_icon(GTK_ENTRY(e->name));
	gtk_entry_set_max_length(GTK_ENTRY(e->name), MAX_NAME_LEN);

	ui_table_add_row(GTK_TABLE(table), 0, label, e->name, NULL);
	
	label = gtk_label_new(_("Base path:"));
	gtk_misc_set_alignment(GTK_MISC(label), 1, 0);

	e->base_path = gtk_entry_new();
	gtk_entry_set_activates_default(GTK_ENTRY(e->base_path), TRUE);
	ui_entry_add_clear_icon(GTK_ENTRY(e->base_path));
	gtk_widget_set_tooltip_text(e->base_path,
		_("Base directory of all files that make up the project. "
		"This can be a new path, or an existing directory tree. "
		"Must be an absolute path."));
	bbox = ui_path_box_new(_("Choose Project Base Path"),
		GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, GTK_ENTRY(e->base_path));

	ui_table_add_row(GTK_TABLE(table), 1, label, bbox, NULL);

	gtk_box_pack_start(GTK_BOX(vbox), table, TRUE, TRUE, 0);

	/* signals */
	g_signal_connect(e->name, "changed", G_CALLBACK(on_name_entry_changed), e);
	/* run the callback manually to initialise the base_path and file_name fields */
	on_name_entry_changed(GTK_EDITABLE(e->name), e);

	g_signal_connect(e->base_path, "changed", G_CALLBACK(on_entries_changed), e);

	gtk_widget_show_all(e->dialog);

	while (gtk_dialog_run(GTK_DIALOG(e->dialog)) == GTK_RESPONSE_OK)
	{
		if (update_config(e, TRUE))
		{
			if (!write_config(app->project,TRUE))
				SHOW_ERR(_("Project file could not be written"));
			else
			{
				ui_set_statusbar(TRUE, _("Project \"%s\" created."), app->project->name);

				sidebar_openfiles_add_project( app->project );
				project_update_list();

				ui_add_recent_project_file(app->project->file_name);

				// write some default files
				gchar *new_filename = g_build_filename( app->project->base_path, "setup.agc", NULL );
				copy_template_file("setup.agc", new_filename);
				g_free(new_filename);

				new_filename = g_build_filename( app->project->base_path, "main.agc", NULL );
				copy_template_file("main.agc", new_filename);
				g_free(new_filename);

				break;
			}
		}
	}
	gtk_widget_destroy(e->dialog);
	g_free(e);
}


gboolean project_load_file_with_session(const gchar *locale_file_name)
{
	if (project_load_file(locale_file_name))
	{
		if (project_prefs.project_session)
		{
			// todo active this when project sessions work
			//configuration_open_files();
		}
		return TRUE;
	}
	return FALSE;
}


//#ifndef G_OS_WIN32
static void run_open_dialog(GtkDialog *dialog)
{
	while (gtk_dialog_run(dialog) == GTK_RESPONSE_ACCEPT)
	{
		gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		
		if ( project_find_by_filename( filename ) )
		{
			gchar *utf8_filename = utils_get_utf8_from_locale(filename);

			SHOW_ERR1(_("Project file \"%s\" is already open"), utf8_filename);
			g_free(utf8_filename);
			g_free(filename);
			continue;
		}

		/* try to load the config */
		if (! project_load_file_with_session(filename))
		{
			gchar *utf8_filename = utils_get_utf8_from_locale(filename);

			SHOW_ERR1(_("Project file \"%s\" could not be loaded."), utf8_filename);
			gtk_widget_grab_focus(GTK_WIDGET(dialog));
			g_free(utf8_filename);
			g_free(filename);
			continue;
		}

		g_free(filename);
		break;
	}
}

static void run_import_dialog(GtkDialog *dialog)
{
	while (gtk_dialog_run(dialog) == GTK_RESPONSE_ACCEPT)
	{
		gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		
		gchar *new_file = g_strdup( filename );
		gchar *ext = strrchr( new_file, '.' );
		if ( ext )
		{
			ext[1] = 'a'; ext[2] = 'g'; ext[3] = 'k'; ext[4] = 0;
		}
		if ( project_find_by_filename( new_file ) )
		{
			gchar *utf8_filename = utils_get_utf8_from_locale(filename);

			SHOW_ERR1(_("Project file \"%s\" is already open"), utf8_filename);
			g_free(utf8_filename);
			g_free(new_file);
			g_free(filename);
			continue;
		}

		/* try to load the config */
		if (! project_import_from_file(filename))
		{
			SHOW_ERR1(_("Project file \"%s\" could not be loaded."), filename);
			g_free(new_file);
			g_free(filename);
			continue;
		}

		g_free(new_file);
		g_free(filename);
		break;
	}
}
//#endif


void project_open(void)
{
	const gchar *dir = local_prefs.project_file_path;
	gchar *file;
	GtkWidget *dialog;
	GtkFileFilter *filter;
	gchar *locale_path;

	
#ifdef G_OS_WIN32
	if (interface_prefs.use_native_windows_dialogs)
	{
		file = win32_show_project_open_dialog(main_widgets.window, _("Open Project"), dir, FALSE, "AGK Project Files (*.agk)\t*.agk\t");
		if (file != NULL)
		{
			if ( project_find_by_filename( file ) )
			{
				gchar *utf8_filename = utils_get_utf8_from_locale(file);

				SHOW_ERR1(_("Project file \"%s\" is already open"), utf8_filename);
				g_free(utf8_filename);
				g_free(file);
				return;
			}

			/* try to load the config */
			if (! project_load_file_with_session(file))
			{
				SHOW_ERR1(_("Project file \"%s\" could not be loaded."), file);
			}
			g_free(file);
		}
	}
	else
#endif
	{
		dialog = gtk_file_chooser_dialog_new(_("Open Project"), GTK_WINDOW(main_widgets.window),
				GTK_FILE_CHOOSER_ACTION_OPEN,
				GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
				GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL);
		gtk_widget_set_name(dialog, "GeanyDialogProject");

		/* set default Open, so pressing enter can open multiple files */
		gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
		gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
		gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dialog), TRUE);
		gtk_window_set_type_hint(GTK_WINDOW(dialog), GDK_WINDOW_TYPE_HINT_DIALOG);
		gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(main_widgets.window));
		gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), FALSE);

		/* add FileFilters */
		filter = gtk_file_filter_new();
		gtk_file_filter_set_name(filter, _("AGK Project files"));
		gtk_file_filter_add_pattern(filter, "*." GEANY_PROJECT_EXT);
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
		gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(dialog), filter);

		locale_path = utils_get_locale_from_utf8(dir);
		if (g_file_test(locale_path, G_FILE_TEST_EXISTS) &&
			g_file_test(locale_path, G_FILE_TEST_IS_DIR))
		{
			gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), locale_path);
		}
		g_free(locale_path);

		gtk_widget_show_all(dialog);
		run_open_dialog(GTK_DIALOG(dialog));
		gtk_widget_destroy(GTK_WIDGET(dialog));
	}
}


void project_import(void)
{
	const gchar *dir = local_prefs.project_file_path;
	gchar *file;
	GtkWidget *dialog;
	GtkFileFilter *filter;
	gchar *locale_path;
	
#ifdef G_OS_WIN32
	if (interface_prefs.use_native_windows_dialogs)
	{
		file = win32_show_project_open_dialog(main_widgets.window, _("Import Project"), dir, FALSE, "Old AGK Projects (*.cbp)\t*.cbp\t");
		if (file != NULL)
		{
			gchar *new_file = g_strdup( file );
			gchar *ext = strrchr( new_file, '.' );
			if ( ext )
			{
				ext[1] = 'a'; ext[2] = 'g'; ext[3] = 'k'; ext[4] = 0;
			}
			if ( project_find_by_filename( new_file ) )
			{
				gchar *utf8_filename = utils_get_utf8_from_locale(file);

				SHOW_ERR1(_("Project file \"%s\" is already open"), utf8_filename);
				g_free(utf8_filename);
				g_free(new_file);
				g_free(file);
				return;
			}

			/* try to load the config */
			if (! project_import_from_file(file))
			{
				SHOW_ERR1(_("Project file \"%s\" could not be loaded."), file);
			}
			g_free(new_file);
			g_free(file);
		}
	}
	else
#endif
	{
		dialog = gtk_file_chooser_dialog_new(_("Import Project"), GTK_WINDOW(main_widgets.window),
				GTK_FILE_CHOOSER_ACTION_OPEN,
				GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
				GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT, NULL);
		gtk_widget_set_name(dialog, "GeanyDialogProject");

		/* set default Open, so pressing enter can open multiple files */
		gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);
		gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
		gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dialog), TRUE);
		gtk_window_set_type_hint(GTK_WINDOW(dialog), GDK_WINDOW_TYPE_HINT_DIALOG);
		gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(main_widgets.window));
		gtk_file_chooser_set_select_multiple(GTK_FILE_CHOOSER(dialog), FALSE);

		/* add FileFilters */
		filter = gtk_file_filter_new();
		gtk_file_filter_set_name(filter, _("Old AGK Projects"));
		gtk_file_filter_add_pattern(filter, "*.cbp");
		gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);
		gtk_file_chooser_set_filter(GTK_FILE_CHOOSER(dialog), filter);

		locale_path = utils_get_locale_from_utf8(dir);
		if (g_file_test(locale_path, G_FILE_TEST_EXISTS) &&
			g_file_test(locale_path, G_FILE_TEST_IS_DIR))
		{
			gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), locale_path);
		}
		g_free(locale_path);

		gtk_widget_show_all(dialog);
		run_import_dialog(GTK_DIALOG(dialog));
		gtk_widget_destroy(GTK_WIDGET(dialog));
	}
}

void project_export_apk()
{
	
}

/* Called when creating, opening, closing and updating projects. */
static void update_ui(void)
{
	if (main_status.quitting)
		return;

	ui_set_window_title(NULL);
	build_menu_update(NULL);
	sidebar_openfiles_update_all();
}

gboolean project_close_all()
{
	gint i;
	for ( i = 0; i < projects_array->len; i++ )
	{
		if ( projects[i]->is_valid )
		{
			if ( !project_close( projects[i], FALSE ) ) return FALSE;
			if ( app->project == projects[i] ) app->project = NULL;
		}
	}

	app->project = NULL;

	return TRUE;
}

/* open_default will make function reload default session files on close */
gboolean project_close(GeanyProject *project, gboolean open_default)
{
	GSList *node;
	guint i;

	if ( project == NULL ) return TRUE;
	if ( !project->is_valid ) return TRUE;

	/* save project session files, etc */
	if (!write_config(project,FALSE))
		g_warning("Project file \"%s\" could not be written", project->file_name);

	if (project_prefs.project_session)
	{
		/* close all existing tabs first */
		if (!document_close_all_project(project))
			return FALSE;
	}
	ui_set_statusbar(TRUE, _("Project \"%s\" closed."), project->name);

	sidebar_remove_project( project );
	
	/* remove project non filetype build menu items */
	//build_remove_menu_item(GEANY_BCS_PROJ, GEANY_GBG_NON_FT, -1);
	//build_remove_menu_item(GEANY_BCS_PROJ, GEANY_GBG_EXEC, -1);

	project->is_valid = FALSE;

	g_free(project->name);
	g_free(project->description);
	g_free(project->file_name);
	g_free(project->base_path);

	// free file and group arrays
	for (i = 0; i < project->project_files->len; i++)
		g_free(project->project_files->pdata[i]);
	g_ptr_array_free(project->project_files, TRUE);

	for (i = 0; i < project->project_groups->len; i++)
		g_free(project->project_groups->pdata[i]);
	g_ptr_array_free(project->project_groups, TRUE);

	//g_free(project);
	memset(project, 0, sizeof(GeanyProject));
	app->project = project_find_first_valid();
	project_update_list();
	ui_project_buttons_update();

	foreach_slist(node, stash_groups)
		stash_group_free(node->data);

	g_slist_free(stash_groups);
	stash_groups = NULL;

	apply_editor_prefs(); /* ensure that global settings are restored */

	if (project_prefs.project_session)
	{
		/* after closing all tabs let's open the tabs found in the default config */
		if (open_default && cl_options.load_session)
		{
			//configuration_reload_default_session();
			//configuration_open_files();
			/* open a new file if no other file was opened */
			//document_new_file_if_non_open();
			//ui_focus_current_document();
		}
	}
	g_signal_emit_by_name(geany_object, "project-close");

	update_ui();

	return TRUE;
}

gint project_get_new_file_idx(GeanyProject *project)
{
	guint i;

	for (i = 0; i < project->project_files->len; i++)
	{
		if ( !project_files_index(project,i)->is_valid )
		{
			return (gint) i;
		}
	}
	return -1;
}

gint project_get_new_group_idx(GeanyProject *project)
{
	guint i;

	for (i = 0; i < project->project_groups->len; i++)
	{
		if ( !project_groups_index(project,i)->is_valid )
		{
			return (gint) i;
		}
	}
	return -1;
}

gboolean project_add_file(GeanyProject *project, gchar* filename, gboolean update_sidebar)
{
	if ( !project )
	{
		SHOW_ERR( "Failed to add file to project, no current project selected. Click Project in the menu bar to create a new project or open an existing one." );
		return FALSE;
	}

	gint new_idx = project_get_new_file_idx( project );
	if (new_idx == -1)	/* expand the array, no free places */
	{
		GeanyProjectFile *file = g_new0(GeanyProjectFile, 1);

		new_idx = project->project_files->len;
		g_ptr_array_add(project->project_files, file);
	}

	GeanyProjectFile *file = project->project_files->pdata[new_idx];
	file->is_valid = TRUE;
	file->file_name = g_strdup( filename );

	if ( update_sidebar )
	{
		if (!write_config(project,TRUE))
			SHOW_ERR(_("Project file could not be saved"));
		else
			ui_set_statusbar(TRUE, _("Project \"%s\" saved."), project->name);

		sidebar_openfiles_add_file( project, filename );
	}

	return TRUE;
}

void project_remove_file(GeanyProject *project, gchar* filename, gboolean update_sidebar)
{
	if ( !project )
	{
		SHOW_ERR( "Failed to remove file from project, no current project selected" );
		return;
	}

	int i;
	for( i = 0; i < project->project_files->len; i++ )
	{
		if ( project_files_index(project,i)->is_valid )
		{
			if ( strcmp( project_files_index(project,i)->file_name, filename ) == 0 )
			{
				g_free( project_files_index(project,i)->file_name );
				project_files_index(project,i)->is_valid = FALSE;
			}
		}
	}
	
	if ( update_sidebar )
	{
		if (!write_config(project,TRUE))
			SHOW_ERR(_("Project file could not be saved"));
		else
			ui_set_statusbar(TRUE, _("Project \"%s\" saved."), project->name);

		sidebar_openfiles_remove_file( project, filename );
		GeanyDocument *doc = document_find_by_filename( filename );
		if ( doc ) 
			sidebar_openfiles_add( doc );
	}
}

/* Shows the file chooser dialog when base path button is clicked
 * FIXME: this should be connected in Glade but 3.8.1 has a bug
 * where it won't pass any objects as user data (#588824). */
G_MODULE_EXPORT void
on_project_properties_base_path_button_clicked(GtkWidget *button,
	GtkWidget *base_path_entry)
{
	GtkWidget *dialog;

	g_return_if_fail(base_path_entry != NULL);
	g_return_if_fail(GTK_IS_WIDGET(base_path_entry));

	dialog = gtk_file_chooser_dialog_new(_("Choose Project Base Path"),
		NULL, GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER,
		GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
		GTK_STOCK_OPEN, GTK_RESPONSE_ACCEPT,
		NULL);

	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
	{
		gtk_entry_set_text(GTK_ENTRY(base_path_entry),
			gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog)));
	}

	gtk_widget_destroy(dialog);
}


static void insert_build_page(PropertyDialogElements *e)
{
	GtkWidget *build_table, *label;
	GeanyDocument *doc = document_get_current();
	GeanyFiletype *ft = NULL;

	if (doc != NULL)
		ft = doc->file_type;

	//build_table = build_commands_table(doc, GEANY_BCS_PROJ, &(e->build_properties), ft);
	//gtk_container_set_border_width(GTK_CONTAINER(build_table), 6);
	//label = gtk_label_new(_("Build"));
	//e->build_page_num = gtk_notebook_append_page(GTK_NOTEBOOK(e->notebook),
	//	build_table, label);
}


static void create_properties_dialog(PropertyDialogElements *e)
{
	GtkWidget *base_path_button;
	static guint base_path_button_handler_id = 0;
	static guint radio_long_line_handler_id = 0;

	e->dialog = create_project_dialog();
	e->notebook = ui_lookup_widget(e->dialog, "project_notebook");
	e->file_name = ui_lookup_widget(e->dialog, "label_project_dialog_filename");
	e->name = ui_lookup_widget(e->dialog, "entry_project_dialog_name");
	e->description = ui_lookup_widget(e->dialog, "textview_project_dialog_description");
	e->base_path = ui_lookup_widget(e->dialog, "entry_project_dialog_base_path");
	e->patterns = ui_lookup_widget(e->dialog, "entry_project_dialog_file_patterns");

	gtk_entry_set_max_length(GTK_ENTRY(e->name), MAX_NAME_LEN);

	ui_entry_add_clear_icon(GTK_ENTRY(e->name));
	ui_entry_add_clear_icon(GTK_ENTRY(e->base_path));
	ui_entry_add_clear_icon(GTK_ENTRY(e->patterns));

	/* Workaround for bug in Glade 3.8.1, see comment above signal handler */
	if (base_path_button_handler_id == 0)
	{
		base_path_button = ui_lookup_widget(e->dialog, "button_project_dialog_base_path");
		base_path_button_handler_id =
			g_signal_connect(base_path_button, "clicked",
			G_CALLBACK(on_project_properties_base_path_button_clicked),
			e->base_path);
	}

	/* Same as above, should be in Glade but can't due to bug in 3.8.1 */
	if (radio_long_line_handler_id == 0)
	{
		radio_long_line_handler_id =
			g_signal_connect(ui_lookup_widget(e->dialog,
			"radio_long_line_custom_project"), "toggled",
			G_CALLBACK(on_radio_long_line_custom_toggled),
			ui_lookup_widget(e->dialog, "spin_long_line_project"));
	}
}


/* checks whether there is an already open project and asks the user if he wants to close it or
 * abort the current action. Returns FALSE when the current action(the caller) should be cancelled
 * and TRUE if we can go ahead */
gboolean project_ask_close(void)
{
	if (app->project != NULL)
	{
		if (dialogs_show_question_full(NULL, GTK_STOCK_CLOSE, GTK_STOCK_CANCEL,
			_("Do you want to close it before proceeding?"),
			_("The '%s' project is open."), app->project->name))
		{
			project_close(app->project,FALSE);
			return TRUE;
		}
		else
			return FALSE;
	}
	else
		return TRUE;
}

static gint project_get_new_idx(void)
{
	guint i;

	for (i = 0; i < projects_array->len; i++)
	{
		if ( !projects[i]->is_valid )
		{
			return (gint) i;
		}
	}
	return -1;
}

static GeanyProject *create_project(void)
{
	GeanyProject *project;
	gint new_idx;

	new_idx = project_get_new_idx();
	if (new_idx == -1)	/* expand the array, no free places */
	{
		project = g_new0(GeanyProject, 1);

		new_idx = projects_array->len;
		g_ptr_array_add(projects_array, project);
	}

	project = projects[new_idx];
	project->index = new_idx;
	project->project_files = g_ptr_array_new();
	project->project_groups = g_ptr_array_new();

	app->project = project;
	return project;
}

gboolean project_import_from_file(const gchar *filename)
{
	GeanyProject *p;
	gchar *file_data;
	GError *err;
	
	g_return_val_if_fail(filename != NULL, FALSE);

	if ( !g_file_get_contents( filename, &file_data, NULL, &err ) )
	{
		ui_set_statusbar(TRUE, "%s", err->message);
		g_error_free(err);
		return FALSE;
	}

	p = create_project();

	//p->name = utils_get_setting_string(config, "project", "name", GEANY_STRING_UNTITLED);
	p->name = g_path_get_basename( filename );
	gchar* dot = strrchr( p->name, '.' );
	if ( dot ) *dot = 0;
	p->description = g_strdup("");
	p->file_name = utils_get_utf8_from_locale(filename);
	dot = strrchr(p->file_name, '.');
	if ( dot ) 
	{
		*(dot+1) = 'a';
		*(dot+2) = 'g';
		*(dot+3) = 'k';
		*(dot+4) = 0;
	}
	//p->base_path = utils_get_setting_string(config, "project", "base_path", "");
	p->base_path = g_strdup( p->file_name );
	gchar* slash = strrchr( p->base_path, '/' );
	gchar* slash2 = strrchr( p->base_path, '\\' );
	if ( slash || slash2 ) 
	{
		if ( slash > slash2 ) 
			*(slash+1) = 0;
		else 
			*(slash2+1) = 0;
	}

	ui_project_buttons_update();
	
	// import project files
	gchar* file_ptr = file_data;
	while( *file_ptr )
	{
		if ( strncmp( file_ptr, "<Unit filename=\"", strlen("<Unit filename=\"") ) == 0 )
		{
			gchar *start = file_ptr + strlen("<Unit filename=\"");
			gchar *end = start;
			while ( *end != '"' && *end ) end++;

			if ( !*end )
			{
				SHOW_ERR(_("Failed to import project source file, project file may be corrupt"));
			}
			else
			{
				gint len = (gint)(end-start);
				if ( len < 1000 )
				{
					gchar source_file[1024];
					strncpy( source_file, start, len );
					source_file[ len ] = 0;
					if ( !g_path_is_absolute(source_file) )
					{
						if ( strlen(p->base_path) + strlen(source_file) < 1000 )
						{
							gchar* tmp = g_strdup(source_file);
							strcpy(source_file, p->base_path);
							strcat(source_file, tmp);
							g_free(tmp);
							utils_tidy_path( source_file );
							//gchar* full_path = utils_get_real_path(source_file);
							//strcpy(source_file, full_path);
							//g_free(full_path);

							project_add_file( p, source_file, FALSE );
						}
					}
					else
						project_add_file( p, source_file, FALSE );
				}
			}

			file_ptr = end;
		}
		else
			file_ptr++;
	}

	p->is_valid = TRUE;

	// save new project file
	if ( !write_config(p,FALSE) )
		SHOW_ERR(_("Project file could not be written"));
	else
		ui_set_statusbar(TRUE, _("Project \"%s\" imported."), p->name);
	
	sidebar_openfiles_add_project( p );
	project_update_list();

	update_ui();

	ui_add_recent_project_file(p->file_name);
	return TRUE;
}

/* Verifies data for New & Properties dialogs.
 * Returns: FALSE if the user needs to change any data. */
static gboolean update_config(const PropertyDialogElements *e, gboolean new_project)
{
	const gchar *name, *file_name, *base_path;
	gchar *locale_filename;
	gsize name_len;
	gint err_code = 0;
	GeanyProject *p;

	g_return_val_if_fail(e != NULL, TRUE);

	name = gtk_entry_get_text(GTK_ENTRY(e->name));
	name_len = strlen(name);
	if (name_len == 0)
	{
		SHOW_ERR(_("The specified project name is too short."));
		gtk_widget_grab_focus(e->name);
		return FALSE;
	}
	else if (name_len > MAX_NAME_LEN)
	{
		SHOW_ERR1(_("The specified project name is too long (max. %d characters)."), MAX_NAME_LEN);
		gtk_widget_grab_focus(e->name);
		return FALSE;
	}

	base_path = gtk_entry_get_text(GTK_ENTRY(e->base_path));
	if (EMPTY(base_path))
	{
		SHOW_ERR(_("The project must have a base path"));
		gtk_widget_grab_focus(e->base_path);
		return FALSE;
	}
	else
	{	/* check whether the given directory actually exists */
		gchar *locale_path = utils_get_locale_from_utf8(base_path);

		if (! g_path_is_absolute(locale_path))
		{	
			SHOW_ERR(_("The project path must be an absolute path"));
			gtk_widget_grab_focus(e->base_path);
			return FALSE;
		}

		if (! g_file_test(locale_path, G_FILE_TEST_IS_DIR))
		{
			gboolean create_dir;

			create_dir = dialogs_show_question_full(NULL, GTK_STOCK_OK, GTK_STOCK_CANCEL,
				_("Create the project's base path directory?"),
				_("The path \"%s\" does not exist."),
				base_path);

			if (create_dir)
				err_code = utils_mkdir(locale_path, TRUE);

			if (! create_dir || err_code != 0)
			{
				if (err_code != 0)
					SHOW_ERR1(_("Project base directory could not be created (%s)."), g_strerror(err_code));
				gtk_widget_grab_focus(e->base_path);
				utils_free_pointers(1, locale_path, NULL);
				return FALSE;
			}
		}
		g_free(locale_path);
	}

	if (new_project)
	{
		// generate project filename from project path and name
		if ( base_path[ strlen(base_path)-1 ] == '/' || base_path[ strlen(base_path)-1 ] == '\\' )
			file_name = g_strconcat(base_path, name, "." GEANY_PROJECT_EXT, NULL);
		else
			file_name = g_strconcat(base_path, G_DIR_SEPARATOR_S, name, "." GEANY_PROJECT_EXT, NULL);
	}
	else
		file_name = gtk_label_get_text(GTK_LABEL(e->file_name));

	if (G_UNLIKELY(EMPTY(file_name)))
	{
		SHOW_ERR(_("You have specified an invalid project filename."));
		gtk_widget_grab_focus(e->file_name);
		return FALSE;
	}

	locale_filename = utils_get_locale_from_utf8(file_name);
		
	/* finally test whether the given project file can be written */
	if ((err_code = utils_is_file_writable(locale_filename)) != 0 ||
		(err_code = g_file_test(locale_filename, G_FILE_TEST_IS_DIR) ? EISDIR : 0) != 0)
	{
		SHOW_ERR1(_("Project file could not be written (%s)."), g_strerror(err_code));
		gtk_widget_grab_focus(e->file_name);
		g_free(locale_filename);
		return FALSE;
	}
	g_free(locale_filename);

	create_project();
	new_project = TRUE;
		
	p = app->project;
	p->is_valid = TRUE;

	SETPTR(p->name, g_strdup(name));
	SETPTR(p->file_name, g_strdup(file_name));
	/* use "." if base_path is empty */
	SETPTR(p->base_path, g_strdup(!EMPTY(base_path) ? base_path : "./"));

	ui_project_buttons_update();	
	update_ui();

	return TRUE;
}


#ifndef G_OS_WIN32
static void run_dialog(GtkWidget *dialog, GtkWidget *entry)
{
	/* set filename in the file chooser dialog */
	const gchar *utf8_filename = gtk_entry_get_text(GTK_ENTRY(entry));
	gchar *locale_filename = utils_get_locale_from_utf8(utf8_filename);

	if (g_path_is_absolute(locale_filename))
	{
		if (g_file_test(locale_filename, G_FILE_TEST_EXISTS))
		{
			/* if the current filename is a directory, we must use
			 * gtk_file_chooser_set_current_folder(which expects a locale filename) otherwise
			 * we end up in the parent directory */
			if (g_file_test(locale_filename, G_FILE_TEST_IS_DIR))
				gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), locale_filename);
			else
				gtk_file_chooser_set_filename(GTK_FILE_CHOOSER(dialog), utf8_filename);
		}
		else /* if the file doesn't yet exist, use at least the current directory */
		{
			gchar *locale_dir = g_path_get_dirname(locale_filename);
			gchar *name = g_path_get_basename(utf8_filename);

			if (g_file_test(locale_dir, G_FILE_TEST_EXISTS))
				gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), locale_dir);
			gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), name);

			g_free(name);
			g_free(locale_dir);
		}
	}
	else if (gtk_file_chooser_get_action(GTK_FILE_CHOOSER(dialog)) != GTK_FILE_CHOOSER_ACTION_OPEN)
	{
		gtk_file_chooser_set_current_name(GTK_FILE_CHOOSER(dialog), utf8_filename);
	}
	g_free(locale_filename);

	/* run it */
	if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_ACCEPT)
	{
		gchar *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
		gchar *tmp_utf8_filename = utils_get_utf8_from_locale(filename);

		gtk_entry_set_text(GTK_ENTRY(entry), tmp_utf8_filename);

		g_free(tmp_utf8_filename);
		g_free(filename);
	}
	gtk_widget_destroy(dialog);
}
#endif


static void on_file_save_button_clicked(GtkButton *button, PropertyDialogElements *e)
{
#ifdef G_OS_WIN32
	gchar *path = win32_show_project_open_dialog(e->dialog, _("Choose Project Filename"),
						gtk_entry_get_text(GTK_ENTRY(e->file_name)), TRUE, "AGK Project Files\t*.agk\t");
	if (path != NULL)
	{
		gtk_entry_set_text(GTK_ENTRY(e->file_name), path);
		g_free(path);
	}
#else
	GtkWidget *dialog;

	/* initialise the dialog */
	dialog = gtk_file_chooser_dialog_new(_("Choose Project Filename"), NULL,
					GTK_FILE_CHOOSER_ACTION_SAVE,
					GTK_STOCK_CANCEL, GTK_RESPONSE_CANCEL,
					GTK_STOCK_SAVE, GTK_RESPONSE_ACCEPT, NULL);
	gtk_widget_set_name(dialog, "GeanyDialogProject");
	gtk_window_set_destroy_with_parent(GTK_WINDOW(dialog), TRUE);
	gtk_window_set_skip_taskbar_hint(GTK_WINDOW(dialog), TRUE);
	gtk_window_set_type_hint(GTK_WINDOW(dialog), GDK_WINDOW_TYPE_HINT_DIALOG);
	gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_ACCEPT);

	run_dialog(dialog, e->file_name);
#endif
}


/* sets the project base path and the project file name according to the project name */
static void on_name_entry_changed(GtkEditable *editable, PropertyDialogElements *e)
{
	gchar *base_path;
	gchar *name;
	const gchar *project_dir = local_prefs.project_file_path;

	if (entries_modified)
		return;

	name = gtk_editable_get_chars(editable, 0, -1);
	if (!EMPTY(name))
	{
		base_path = g_strconcat(project_dir, G_DIR_SEPARATOR_S, name, G_DIR_SEPARATOR_S, NULL);
	}
	else
	{
		base_path = g_strconcat(project_dir, G_DIR_SEPARATOR_S, NULL);
	}
	g_free(name);

	gtk_entry_set_text(GTK_ENTRY(e->base_path), base_path);

	entries_modified = FALSE;

	g_free(base_path);
}


static void on_entries_changed(GtkEditable *editable, PropertyDialogElements *e)
{
	entries_modified = TRUE;
}


static void on_radio_long_line_custom_toggled(GtkToggleButton *radio, GtkWidget *spin_long_line)
{
	gtk_widget_set_sensitive(spin_long_line, gtk_toggle_button_get_active(radio));
}


gboolean project_load_file(const gchar *locale_file_name)
{
	g_return_val_if_fail(locale_file_name != NULL, FALSE);

	if (load_config(locale_file_name))
	{
		gchar *utf8_filename = utils_get_utf8_from_locale(locale_file_name);

		ui_set_statusbar(TRUE, _("Project \"%s\" opened."), app->project->name);

		ui_add_recent_project_file(utf8_filename);
		g_free(utf8_filename);



		return TRUE;
	}
	else
	{
		gchar *utf8_filename = utils_get_utf8_from_locale(locale_file_name);

		ui_set_statusbar(TRUE, _("Project file \"%s\" could not be loaded."), utf8_filename);
		g_free(utf8_filename);
	}
	return FALSE;
}


/* Reads the given filename and creates a new project with the data found in the file.
 * At this point there should not be an already opened project in Geany otherwise it will just
 * return.
 * The filename is expected in the locale encoding. */
static gboolean load_config(const gchar *filename)
{
	GKeyFile *config;
	GeanyProject *p;
	GSList *node;

	g_return_val_if_fail(filename != NULL, FALSE);

	config = g_key_file_new();
	if (! g_key_file_load_from_file(config, filename, G_KEY_FILE_NONE, NULL))
	{
		g_key_file_free(config);
		return FALSE;
	}

	p = create_project();

	foreach_slist(node, stash_groups)
		stash_group_load_from_key_file(node->data, config);

	//p->name = utils_get_setting_string(config, "project", "name", GEANY_STRING_UNTITLED);
	p->name = g_path_get_basename( filename );
	gchar* dot = strrchr( p->name, '.' );
	if ( dot ) *dot = 0;
	p->description = utils_get_setting_string(config, "project", "description", "");
	p->file_name = utils_get_utf8_from_locale(filename);
	//p->base_path = utils_get_setting_string(config, "project", "base_path", "");
	p->base_path = g_strdup( p->file_name );
	gchar* slash = strrchr( p->base_path, '/' );
	gchar* slash2 = strrchr( p->base_path, '\\' );
	if ( slash || slash2 ) 
	{
		if ( slash > slash2 ) 
			*(slash+1) = 0;
		else 
			*(slash2+1) = 0;
	}

	ui_project_buttons_update();
	
	configuration_load_project_files(config, p);

	p->is_valid = TRUE;

	sidebar_openfiles_add_project( p );
	project_update_list();

	if (project_prefs.project_session)
	{
		/* read session files so they can be opened with configuration_open_files() */
		configuration_load_session_files(config, p);
	}

	g_signal_emit_by_name(geany_object, "project-open", config);
	g_key_file_free(config);

	update_ui();
	return TRUE;
}

static void apply_editor_prefs(void)
{
	guint i;

	foreach_document(i)
		editor_apply_update_prefs(documents[i]->editor);
}


/* Write the project settings as well as the project session files into its configuration files.
 * emit_signal defines whether the project-save signal should be emitted. When write_config()
 * is called while closing a project, this is used to skip emitting the signal because
 * project-close will be emitted afterwards.
 * Returns: TRUE if project file was written successfully. */
static gboolean write_config(GeanyProject *project, gboolean emit_signal)
{
	GeanyProject *p;
	GKeyFile *config;
	gchar *filename;
	gchar *data;
	gboolean ret = FALSE;
	GSList *node;

	g_return_val_if_fail(app->project != NULL, FALSE);

	p = app->project;

	config = g_key_file_new();
	/* try to load an existing config to keep manually added comments */
	filename = utils_get_locale_from_utf8(p->file_name);
	g_key_file_load_from_file(config, filename, G_KEY_FILE_NONE, NULL);

	foreach_slist(node, stash_groups)
		stash_group_save_to_key_file(node->data, config);

	//g_key_file_set_string(config, "project", "name", p->name);
	//g_key_file_set_string(config, "project", "base_path", p->base_path);

	if (p->description)
		g_key_file_set_string(config, "project", "description", p->description);

	configuration_save_project_files(config,p);
	
	/* store the session files into the project too */
	if (project_prefs.project_session)
		configuration_save_session_files(config,p);
	
	if (emit_signal)
	{
		g_signal_emit_by_name(geany_object, "project-save", config);
	}
	/* write the file */
	data = g_key_file_to_data(config, NULL, NULL);
	ret = (utils_write_file(filename, data) == 0);

	g_free(data);
	g_free(filename);
	g_key_file_free(config);

	return ret;
}


/* Constructs the project's base path which is used for "Make all" and "Execute".
 * The result is an absolute string in UTF-8 encoding which is either the same as
 * base path if it is absolute or it is built out of project file name's dir and base_path.
 * If there is no project or project's base_path is invalid, NULL will be returned.
 * The returned string should be freed when no longer needed. */
gchar *project_get_base_path(void)
{
	GeanyProject *project = app->project;

	if (project && !EMPTY(project->base_path))
	{
		if (g_path_is_absolute(project->base_path))
			return g_strdup(project->base_path);
		else
		{	/* build base_path out of project file name's dir and base_path */
			gchar *path;
			gchar *dir = g_path_get_dirname(project->file_name);

			if (utils_str_equal(project->base_path, "./"))
				return dir;

			path = g_build_filename(dir, project->base_path, NULL);
			g_free(dir);
			return path;
		}
	}
	return NULL;
}


/* This is to save project-related global settings, NOT project file settings. */
void project_save_prefs(GKeyFile *config)
{
	GeanyProject *project = app->project;

	if (cl_options.load_session)
	{
		const gchar *utf8_filename = (project == NULL) ? "" : project->file_name;

		g_key_file_set_string(config, "project", "session_file", utf8_filename);
	}
	g_key_file_set_string(config, "project", "project_file_path",
		FALLBACK(local_prefs.project_file_path, ""));
}


void project_load_prefs(GKeyFile *config)
{
	if (cl_options.load_session)
	{
		g_return_if_fail(project_prefs.session_file == NULL);
		project_prefs.session_file = utils_get_setting_string(config, "project",
			"session_file", "");
	}
	local_prefs.project_file_path = utils_get_setting_string(config, "project",
		"project_file_path", NULL);
	
	if (local_prefs.project_file_path == NULL)
	{
		local_prefs.project_file_path = g_build_filename(g_get_home_dir(), "AGK Projects", NULL);
	}

	/*
	if (local_prefs.project_file_path == NULL)
	{
		//local_prefs.project_file_path = g_build_filename(g_get_home_dir(), PROJECT_DIR, NULL);
		gchar *path;
#ifdef G_OS_WIN32
		path = win32_get_installation_dir();
#else
		path = g_strdup(GEANY_DATADIR);
#endif
		local_prefs.project_file_path = g_build_filename(path, "../../Projects", NULL);
		utils_tidy_path( local_prefs.project_file_path );
	}
	*/
}


/* Initialize project-related preferences in the Preferences dialog. */
void project_setup_prefs(void)
{
	GtkWidget *path_entry = ui_lookup_widget(ui_widgets.prefs_dialog, "project_file_path_entry");
	GtkWidget *path_btn = ui_lookup_widget(ui_widgets.prefs_dialog, "project_file_path_button");
	static gboolean callback_setup = FALSE;

	g_return_if_fail(local_prefs.project_file_path != NULL);

	gtk_entry_set_text(GTK_ENTRY(path_entry), local_prefs.project_file_path);
	if (! callback_setup)
	{	/* connect the callback only once */
		callback_setup = TRUE;
		ui_setup_open_button_callback(path_btn, NULL,
			GTK_FILE_CHOOSER_ACTION_SELECT_FOLDER, GTK_ENTRY(path_entry));
	}
}


/* Update project-related preferences after using the Preferences dialog. */
void project_apply_prefs(void)
{
	GtkWidget *path_entry = ui_lookup_widget(ui_widgets.prefs_dialog, "project_file_path_entry");
	const gchar *str;

	str = gtk_entry_get_text(GTK_ENTRY(path_entry));
	SETPTR(local_prefs.project_file_path, g_strdup(str));
}


static void add_stash_group(StashGroup *group)
{
	stash_groups = g_slist_prepend(stash_groups, group);
}

const GeanyFilePrefs *project_get_file_prefs(void)
{
	return &file_prefs;
}

static gint combo_sort_func(GtkTreeModel *model, GtkTreeIter *iter_a,
								GtkTreeIter *iter_b, gpointer data)
{
	gchar *name_a, *name_b;
	gint cmp;
	
	gtk_tree_model_get(model, iter_a, 0, &name_a, -1);
	gtk_tree_model_get(model, iter_b, 0, &name_b, -1);
	cmp = strcmp(name_a, name_b);
	
	g_free(name_a);
	g_free(name_b);

	return cmp;
}

void project_init(void)
{
	GtkTreeIter iter;

	projects_array = g_ptr_array_new();

	project_choice = ui_lookup_widget(main_widgets.window, "combobox1");
	project_choice_container = ui_lookup_widget(main_widgets.window, "hbox4");

	gtk_widget_hide( project_choice_container );

	GtkListStore *list = gtk_list_store_new( 2, G_TYPE_STRING, G_TYPE_POINTER );
	
	/*
	// test items
	gtk_list_store_append( list, &iter );
	gtk_list_store_set( list, &iter, 0, "None", 1, 0, -1 );
	gtk_list_store_append( list, &iter );
	gtk_list_store_set( list, &iter, 0, "First", 1, 0, -1 );
	*/

	gtk_combo_box_set_model( GTK_COMBO_BOX(project_choice), GTK_TREE_MODEL(list) );
	g_object_unref( G_OBJECT(list) );

	GtkTreeSortable *sortable = GTK_TREE_SORTABLE(GTK_TREE_MODEL(list));
	gtk_tree_sortable_set_sort_func(sortable, 0, combo_sort_func, NULL, NULL);
	gtk_tree_sortable_set_sort_column_id(sortable, 0, GTK_SORT_ASCENDING);
}

GtkTreeIter* get_combo_iter( GeanyProject *project )
{
	GtkTreeModel *model = gtk_combo_box_get_model( GTK_COMBO_BOX(project_choice) );
	static GtkTreeIter iter;
	GeanyProject *other_project;

	if ( !gtk_tree_model_get_iter_first( model, &iter ) )
		return NULL;

	do
	{
		gtk_tree_model_get( model, &iter, 1, &other_project, -1 );
		if ( project == other_project ) return &iter;
	} while( gtk_tree_model_iter_next( model, &iter ) );

	return NULL;
}

void project_combo_add( GeanyProject *project )
{
	GtkTreeIter iter;
	GtkListStore *list = GTK_LIST_STORE( gtk_combo_box_get_model( GTK_COMBO_BOX(project_choice) ) );

	if ( get_combo_iter( project ) ) 
		return;

	gtk_list_store_append( list, &iter );
	gtk_list_store_set( list, &iter, 0, project->name, 1, project, -1 );
}

void project_update_list(void)
{
	GtkListStore *list = GTK_LIST_STORE( gtk_combo_box_get_model( GTK_COMBO_BOX(project_choice) ) );
	gtk_list_store_clear( list );	

	if ( projects_array->len < 2 )
		gtk_widget_hide( project_choice_container );
	else
	{
		int i;
		int count = 0;
		for( i = 0; i < projects_array->len; i++ )
		{
			if ( projects[i]->is_valid )
			{
				project_combo_add( projects[i] );
				count++;
			}
		}

		if ( count < 2 )
			gtk_widget_hide( project_choice_container );
		else
		{
			GtkTreeIter *iter = get_combo_iter( app->project );
		
			if ( !iter )
				return;

			gtk_combo_box_set_active_iter( GTK_COMBO_BOX(project_choice), iter );

			gtk_widget_show( project_choice_container );
		}
	}	
}

void project_finalize(void)
{
	guint i;

	for (i = 0; i < projects_array->len; i++)
		g_free(projects[i]);
	g_ptr_array_free(projects_array, TRUE);
}

GeanyProject* project_find_by_filename(const gchar *filename)
{
	guint i;

	if ( ! filename)
		return NULL;	/* file doesn't exist on disk */

	for (i = 0; i < projects_array->len; i++)
	{
		GeanyProject *project = projects[i];

		if (! project->is_valid || ! project->file_name)
			continue;

		if (utils_filenamecmp(filename, project->file_name) == 0)
		{
			return project;
		}
	}
	return NULL;
}

GeanyProject* project_find_first_valid()
{
	guint i;

	for (i = 0; i < projects_array->len; i++)
	{
		GeanyProject *project = projects[i];

		if (! project->is_valid || ! project->file_name)
			continue;

		return project;
	}
	return NULL;
}