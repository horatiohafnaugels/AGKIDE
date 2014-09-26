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

#include "miniz.h"

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
				/*
				gchar *new_filename = g_build_filename( app->project->base_path, "setup.agc", NULL );
				if ( g_file_test( new_filename, G_FILE_TEST_EXISTS ) == 0 )
					copy_template_file("setup.agc", new_filename);
				g_free(new_filename);
				*/

				gchar *new_filename = g_build_filename( app->project->base_path, "main.agc", NULL );
				if ( g_file_test( new_filename, G_FILE_TEST_EXISTS ) == 0 )
					copy_template_file("main.agc", new_filename);
				else
					project_add_file( app->project, new_filename, TRUE );
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

static void on_android_dialog_response(GtkDialog *dialog, gint response, gpointer user_data)
{
	static int running = 0;
	if ( running ) return;

	running = 1;

	if ( response != 1 )
	{
		gtk_widget_hide(GTK_WIDGET(dialog));
	}
	else
	{
		int i;
		GtkWidget *widget;

		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.android_dialog, "android_export1"), FALSE );
		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.android_dialog, "button7"), FALSE );
		
		while (gtk_events_pending())
			gtk_main_iteration();

		// app details
		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_app_name_entry");
		gchar *app_name = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));
		
		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_package_name_entry");
		gchar *package_name = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_app_icon_entry");
		gchar *app_icon = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_ouya_icon_entry");
		gchar *ouya_icon = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_orientation_combo");
		gchar *app_orientation = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widget));
		int orientation = 10;
		if ( strcmp(app_orientation,"Landscape") == 0 ) orientation = 6;
		else if ( strcmp(app_orientation,"Portrait") == 0 ) orientation = 7;
		g_free(app_orientation);
		gchar szOrientation[ 20 ];
		sprintf( szOrientation, "%d", orientation );
		
		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_sdk_combo");
		gchar *app_sdk = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widget));
		int sdk = 10;
		if ( strcmp(app_sdk,"3.2") == 0 ) sdk = 13;
		g_free(app_sdk);
		gchar szSDK[ 20 ];
		sprintf( szSDK, "%d", sdk );

		// permissions
		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_permission_external_storage");
		int permission_external_storage = gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(widget) );

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_permission_location_fine");
		int permission_location_fine = gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(widget) );

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_permission_location_coarse");
		int permission_location_coarse = gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(widget) );

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_permission_internet");
		int permission_internet = gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(widget) );

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_permission_wake");
		int permission_wake = gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(widget) );

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_permission_billing");
		int permission_billing = gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(widget) );

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_permission_push_notifications");
		int permission_push = gtk_toggle_button_get_active( GTK_TOGGLE_BUTTON(widget) );

		// signing
		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_keystore_file_entry");
		gchar *keystore_file = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));
		
		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_keystore_password_entry");
		gchar *keystore_password = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_version_number_entry");
		gchar *version_number = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));
		if ( !*version_number ) SETPTR( version_number, g_strdup("1.0.0") );

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_build_number_entry");
		int build_number = atoi(gtk_entry_get_text(GTK_ENTRY(widget)));
		if ( build_number == 0 ) build_number = 1;
		gchar szBuildNum[ 20 ];
		sprintf( szBuildNum, "%d", build_number );

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_alias_entry");
		gchar *alias_name = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_alias_password_entry");
		gchar *alias_password = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		// output
		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_output_file_entry");
		gchar *output_file = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		widget = ui_lookup_widget(ui_widgets.android_dialog, "android_output_type_combo");
		gchar *output_type = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widget));
		int app_type = 0;
		if ( strcmp(output_type,"Amazon") == 0 ) app_type = 1;
		else if ( strcmp(output_type,"Ouya") == 0 ) app_type = 2;
		g_free(output_type);

		// START CHECKS

		if ( !output_file || !*output_file ) { SHOW_ERR("You must choose an output location to save your APK"); goto android_dialog_clean_up; }

		// check app name
		if ( !app_name || !*app_name ) { SHOW_ERR("You must enter an app name"); goto android_dialog_clean_up; }
		if ( strlen(app_name) > 30 ) { SHOW_ERR("App name must be less than 30 characters"); goto android_dialog_clean_up; }
		for( i = 0; i < strlen(app_name); i++ )
		{
			if ( (app_name[i] < 97 || app_name[i] > 122)
			  && (app_name[i] < 65 || app_name[i] > 90) 
			  && (app_name[i] < 48 || app_name[i] > 57) 
			  && app_name[i] != 32 
			  && app_name[i] != 95 ) 
			{ 
				SHOW_ERR("App name contains invalid characters, must be A-Z 0-9 spaces and undersore only"); 
				goto android_dialog_clean_up; 
			}
		}
		
		// check package name
		if ( !package_name || !*package_name ) { SHOW_ERR("You must enter a package name"); goto android_dialog_clean_up; }
		if ( strlen(package_name) > 50 ) { SHOW_ERR("Package name must be less than 50 characters"); goto android_dialog_clean_up; }
		if ( strchr(package_name,'.') == NULL ) { SHOW_ERR("Package name must contain at least one dot character"); goto android_dialog_clean_up; }
		if ( package_name[0] == '.' || package_name[strlen(package_name)-1] == '.' ) { SHOW_ERR("Package name must not begin or end with a dot"); goto android_dialog_clean_up; }

		for( i = 0; i < strlen(package_name); i++ )
		{
			if ( (package_name[i] < 97 || package_name[i] > 122)
			  && (package_name[i] < 65 || package_name[i] > 90) 
			  && (package_name[i] < 48 || package_name[i] > 57) 
			  && package_name[i] != 46 
			  && package_name[i] != 95 ) 
			{ 
				SHOW_ERR("Package name contains invalid characters, must be A-Z 0-9 . and undersore only"); 
				goto android_dialog_clean_up; 
			}
		}

		// check icon
		//if ( !app_icon || !*app_icon ) { SHOW_ERR("You must select an app icon"); goto android_dialog_clean_up; }
		if ( app_icon && *app_icon )
		{
			if ( !strrchr( app_icon, '.' ) || utils_str_casecmp( strrchr( app_icon, '.' ), ".png" ) != 0 ) { SHOW_ERR("App icon must be a PNG file"); goto android_dialog_clean_up; }
			if ( !g_file_test( app_icon, G_FILE_TEST_EXISTS ) ) { SHOW_ERR("Could not find app icon location"); goto android_dialog_clean_up; }
		}

		if ( app_type == 2 )
		{
			if ( !ouya_icon || !*ouya_icon ) { SHOW_ERR("You must select an Ouya large icon"); goto android_dialog_clean_up; }
			if ( !strrchr( ouya_icon, '.' ) || utils_str_casecmp( strrchr( ouya_icon, '.' ), ".png" ) != 0 ) { SHOW_ERR("Ouya large icon must be a PNG file"); goto android_dialog_clean_up; }
			if ( !g_file_test( ouya_icon, G_FILE_TEST_EXISTS ) ) { SHOW_ERR("Could not find ouya large icon location"); goto android_dialog_clean_up; }
		}
				
		// check version
		if ( version_number && *version_number )
		{
			for( i = 0; i < strlen(version_number); i++ )
			{
				if ( (version_number[i] < 48 || version_number[i] > 57) && version_number[i] != 46 ) 
				{ 
					SHOW_ERR("Version number contains invalid characters, must be 0-9 and . only"); 
					goto android_dialog_clean_up; 
				}
			}
		}

		// check keystore
		if ( keystore_file && *keystore_file )
		{
			if ( !g_file_test( keystore_file, G_FILE_TEST_EXISTS ) ) { SHOW_ERR("Could not find keystore file location"); goto android_dialog_clean_up; }
		}

		// check passwords
		if ( keystore_password && strchr(keystore_password,'"') ) { SHOW_ERR("Keystore password cannot contain double quotes"); goto android_dialog_clean_up; }
		if ( alias_password && strchr(alias_password,'"') ) { SHOW_ERR("Alias password cannot contain double quotes"); goto android_dialog_clean_up; }

		if ( keystore_file && *keystore_file )
		{
			if ( !keystore_password || !*keystore_password ) { SHOW_ERR("You must enter your keystore password when using your own keystore"); goto android_dialog_clean_up; }
		}

		if ( alias_name && *alias_name )
		{
			if ( !alias_password || !*alias_password ) { SHOW_ERR("You must enter your alias password when using a custom alias"); goto android_dialog_clean_up; }
		}

		goto android_dialog_continue;

android_dialog_clean_up:
		if ( app_name ) g_free(app_name);
		if ( package_name ) g_free(package_name);
		if ( app_icon ) g_free(app_icon);

		if ( keystore_file ) g_free(keystore_file);
		if ( keystore_password ) g_free(keystore_password);
		if ( version_number ) g_free(version_number);
		if ( alias_name ) g_free(alias_name);
		if ( alias_password ) g_free(alias_password);

		if ( output_file ) g_free(output_file);

		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.android_dialog, "android_export1"), TRUE );
		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.android_dialog, "button7"), TRUE );
		running = 0;
		return;

android_dialog_continue:

		while (gtk_events_pending())
			gtk_main_iteration();

		// CHECKS COMPLETE, START EXPORT

#ifdef G_OS_WIN32
		gchar* path_to_aapt = g_build_path( "/", app->datadir, "android", "aapt.exe", NULL );
		gchar* path_to_android_jar = g_build_path( "/", app->datadir, "android", "android13.jar", NULL );
		gchar* path_to_jarsigner = g_build_path( "/", app->datadir, "android", "jre", "bin", "jarsigner.exe", NULL );
		gchar* path_to_zipalign = g_build_path( "/", app->datadir, "android", "zipalign.exe", NULL );
#else
        gchar* path_to_aapt = g_build_path( "/", app->datadir, "android", "aapt", NULL );
		gchar* path_to_android_jar = g_build_path( "/", app->datadir, "android", "android13.jar", NULL );
		//gchar* path_to_jarsigner = g_build_path( "/", "/usr", "bin", "jarsigner", NULL );
        gchar* path_to_jarsigner = g_build_path( "/", app->datadir, "android", "jre", "bin", "jarsigner", NULL );
		gchar* path_to_zipalign = g_build_path( "/", app->datadir, "android", "zipalign", NULL );
#endif

		// make temporary folder
		gchar* android_folder = g_build_filename( app->datadir, "android", NULL );
		gchar* tmp_folder = g_build_filename( app->project->base_path, "build_tmp", NULL );

		utils_str_replace_char( android_folder, '\\', '/' );
		utils_str_replace_char( tmp_folder, '\\', '/' );
		
		gchar* src_folder;
		if ( app_type == 2 ) src_folder = g_build_path( "/", app->datadir, "android", "sourceOuya", NULL );
		else if ( app_type == 1 ) src_folder = g_build_path( "/", app->datadir, "android", "sourceAmazon", NULL );
		else src_folder = g_build_path( "/", app->datadir, "android", "sourceGoogle", NULL );
		utils_str_replace_char( src_folder, '\\', '/' );

		gchar *output_file_zip = g_strdup( output_file );
		gchar *ext = strrchr( output_file_zip, '.' );
		if ( ext ) *ext = 0;
		SETPTR( output_file_zip, g_strconcat( output_file_zip, ".zip", NULL ) );

		if ( !keystore_file || !*keystore_file )
		{
			if ( keystore_file ) g_free(keystore_file);
			if ( keystore_password ) g_free(keystore_password);

			keystore_file = g_build_path( "/", app->datadir, "android", "debug.keystore", NULL );
			keystore_password = g_strdup("android");

			if ( alias_name ) g_free(alias_name);
			if ( alias_password ) g_free(alias_password);

			alias_name = g_strdup("androiddebugkey");
			alias_password = g_strdup("android");
		}
		else
		{
			if ( !alias_name || !*alias_name )
			{
				if ( alias_name ) g_free(alias_name);
				if ( alias_password ) g_free(alias_password);

				alias_name = g_strdup("mykeystore");
				alias_password = g_strdup(keystore_password);
			}
		}

		// decalrations
		gchar newcontents[ 32000 ];
		gchar* manifest_file = NULL;
		gchar *contents = NULL;
		gchar *contents2 = NULL;
		gsize length = 0;
		gchar* resources_file = NULL;
		GError *error = NULL;
		GdkPixbuf *icon_image = NULL;
		gchar *image_filename = NULL;
		GdkPixbuf *icon_scaled_image = NULL;
		gchar **argv = NULL;
		gchar **argv2 = NULL;
		gchar **argv3 = NULL;
		gint status = 0;
		mz_zip_archive zip_archive;
		memset(&zip_archive, 0, sizeof(zip_archive));
		gchar *zip_add_file = 0;
		gchar *str_out = NULL;

		if ( !utils_copy_folder( src_folder, tmp_folder, TRUE ) )
		{
			SHOW_ERR( "Failed to copy source folder" );
			goto android_dialog_cleanup2;
		}

		while (gtk_events_pending())
			gtk_main_iteration();
		
		// edit AndroidManifest.xml
		manifest_file = g_build_path( "/", tmp_folder, "AndroidManifest.xml", NULL );

		if ( !g_file_get_contents( manifest_file, &contents, &length, NULL ) )
		{
			SHOW_ERR( "Failed to read AndroidManifest.xml file" );
			goto android_dialog_cleanup2;
		}

		contents2 = strstr( contents, "screenOrientation=\"fullSensor\"" );
		*contents2 = 0;
		contents2 += strlen("screenOrientation=\"fullSensor");
		
		strcpy( newcontents, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n\
<manifest xmlns:android=\"http://schemas.android.com/apk/res/android\"\n\
      android:versionCode=\"" );
		strcat( newcontents, szBuildNum );
		strcat( newcontents, "\"\n      android:versionName=\"" );
		strcat( newcontents, version_number );
		strcat( newcontents, "\" package=\"" );
		strcat( newcontents, package_name );
		strcat( newcontents, "\" android:installLocation=\"auto\">\n\
    <uses-feature android:glEsVersion=\"0x00020000\"></uses-feature>\n\
    <uses-sdk android:minSdkVersion=\"" );
		strcat( newcontents, szSDK );
		strcat( newcontents, "\" android:targetSdkVersion=\"" );
		strcat( newcontents, szSDK );
		strcat( newcontents, "\" />\n\n" );

		if ( permission_external_storage ) strcat( newcontents, "    <uses-permission android:name=\"android.permission.WRITE_EXTERNAL_STORAGE\"></uses-permission>\n" );
		if ( permission_internet ) 
		{
			strcat( newcontents, "    <uses-permission android:name=\"android.permission.INTERNET\"></uses-permission>\n" );
			strcat( newcontents, "    <uses-permission android:name=\"android.permission.ACCESS_NETWORK_STATE\"></uses-permission>\n" );
			strcat( newcontents, "    <uses-permission android:name=\"android.permission.ACCESS_WIFI_STATE\"></uses-permission>\n" );
		}
		if ( permission_wake ) strcat( newcontents, "    <uses-permission android:name=\"android.permission.WAKE_LOCK\"></uses-permission>\n" );
		if ( permission_location_coarse && app_type == 0 ) strcat( newcontents, "    <uses-permission android:name=\"android.permission.ACCESS_LOCATION_COARSE\"></uses-permission>\n" );
		if ( permission_location_fine && app_type == 0 ) strcat( newcontents, "    <uses-permission android:name=\"android.permission.ACCESS_LOCATION_FINE\"></uses-permission>\n" );
		if ( permission_billing && app_type == 0 ) strcat( newcontents, "    <uses-permission android:name=\"com.android.vending.BILLING\"></uses-permission>\n" );
		if ( permission_push && app_type == 0 ) 
		{
			strcat( newcontents, "    <uses-permission android:name=\"com.google.android.c2dm.permission.RECEIVE\" />\n" );
			strcat( newcontents, "    <permission android:name=\"" );
			strcat( newcontents, package_name );
			strcat( newcontents, ".permission.C2D_MESSAGE\" android:protectionLevel=\"signature\" />\n" );
			strcat( newcontents, "    <uses-permission android:name=\"" );
			strcat( newcontents, package_name );
			strcat( newcontents, ".permission.C2D_MESSAGE\" />\n" );
		}

		strcat( newcontents, contents );

		switch( orientation )
		{
			case 6: strcat( newcontents, "screenOrientation=\"sensorLandscape" ); break;
			case 7: strcat( newcontents, "screenOrientation=\"sensorPortait" ); break;
			default: strcat( newcontents, "screenOrientation=\"fullSensor" ); break;
		}

		strcat( newcontents, contents2 );
	
		// write new Android Manifest.xml file
		if ( !g_file_set_contents( manifest_file, newcontents, strlen(newcontents), &error ) )
		{
			SHOW_ERR1( "Failed to write AndroidManifest.xml file: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto android_dialog_cleanup2;
		}
				
		// write resources file
		strcpy( newcontents, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<resources>\n    <string name=\"app_name\">" );
		strcat( newcontents, app_name );
		strcat( newcontents, "</string>\n    <string name=\"backtext\">Press back to return to the app</string>\n    <string name=\"waittext\">Please Wait...</string>\n</resources>\n" );

		resources_file = g_build_path( "/", tmp_folder, "res", "values", "strings.xml", NULL );

		if ( !g_file_set_contents( resources_file, newcontents, strlen(newcontents), &error ) )
		{
			SHOW_ERR1( "Failed to write resource strings.xml file: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto android_dialog_cleanup2;
		}

		// load icon file
		if ( app_icon && *app_icon )
		{
			icon_image = gdk_pixbuf_new_from_file( app_icon, &error );
			if ( !icon_image || error )
			{
				SHOW_ERR1( "Failed to load image icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto android_dialog_cleanup2;
			}

			// scale it and save it
			// 96x96
			image_filename = g_build_path( "/", tmp_folder, "res", "drawable-xhdpi", "icon.png", NULL );
			icon_scaled_image = gdk_pixbuf_scale_simple( icon_image, 96, 96, GDK_INTERP_HYPER );
			if ( !gdk_pixbuf_save( icon_scaled_image, image_filename, "png", &error, "compression", "9", NULL ) )
			{
				SHOW_ERR1( "Failed to save xhdpi icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto android_dialog_cleanup2;
			}
			gdk_pixbuf_unref( icon_scaled_image );
			g_free( image_filename );

			// 72x72
			image_filename = g_build_path( "/", tmp_folder, "res", "drawable-hdpi", "icon.png", NULL );
			icon_scaled_image = gdk_pixbuf_scale_simple( icon_image, 72, 72, GDK_INTERP_HYPER );
			if ( !gdk_pixbuf_save( icon_scaled_image, image_filename, "png", &error, "compression", "9", NULL ) )
			{
				SHOW_ERR1( "Failed to save hdpi icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto android_dialog_cleanup2;
			}
			gdk_pixbuf_unref( icon_scaled_image );
			g_free( image_filename );

			// 48x48
			image_filename = g_build_path( "/", tmp_folder, "res", "drawable-mdpi", "icon.png", NULL );
			icon_scaled_image = gdk_pixbuf_scale_simple( icon_image, 48, 48, GDK_INTERP_HYPER );
			if ( !gdk_pixbuf_save( icon_scaled_image, image_filename, "png", &error, "compression", "9", NULL ) )
			{
				SHOW_ERR1( "Failed to save mdpi icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto android_dialog_cleanup2;
			}
			gdk_pixbuf_unref( icon_scaled_image );
			g_free( image_filename );

			// 36x36
			image_filename = g_build_path( "/", tmp_folder, "res", "drawable-ldpi", "icon.png", NULL );
			icon_scaled_image = gdk_pixbuf_scale_simple( icon_image, 36, 36, GDK_INTERP_HYPER );
			if ( !gdk_pixbuf_save( icon_scaled_image, image_filename, "png", &error, "compression", "9", NULL ) )
			{
				SHOW_ERR1( "Failed to save ldpi icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto android_dialog_cleanup2;
			}
					
			gdk_pixbuf_unref( icon_scaled_image );
			icon_scaled_image = NULL;

			g_free( image_filename );
			image_filename = NULL;
		}

		// load ouya icon and check size
		if ( app_type == 2 )
		{
			icon_image = gdk_pixbuf_new_from_file( ouya_icon, &error );
			if ( !icon_image || error )
			{
				SHOW_ERR1( "Failed to load Ouya large icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto android_dialog_cleanup2;
			}

			if ( gdk_pixbuf_get_width( icon_image ) != 732 || gdk_pixbuf_get_height( icon_image ) != 412 )
			{
				SHOW_ERR( "Ouya large icon must be 732x412 pixels" );
				goto android_dialog_cleanup2;
			}

			// copy it to the res folder
			image_filename = g_build_path( "/", tmp_folder, "res", "drawable-xhdpi", "ouya_icon.png", NULL );
			utils_copy_file( ouya_icon, image_filename, TRUE );
		}

		while (gtk_events_pending())
			gtk_main_iteration();
							
		// package manifest and resources
		argv = g_new0( gchar*, 17 );
		argv[0] = g_strdup( path_to_aapt );
		argv[1] = g_strdup("package");
		argv[2] = g_strdup("-f");
		argv[3] = g_strdup("-M");
		argv[4] = g_build_path( "/", tmp_folder, "AndroidManifest.xml", NULL );
		argv[5] = g_strdup("-I");
		argv[6] = g_strdup( path_to_android_jar );
		argv[7] = g_strdup("-S");
		argv[8] = g_build_path( "/", tmp_folder, "res", NULL );
		if ( app_type == 2 )
		{
			argv[9] = g_strdup("-F");
			argv[10] = g_strdup( output_file );
			argv[11] = g_strdup("--auto-add-overlay");
			argv[12] = NULL;
		}
		else
		{
			argv[9] = g_strdup("-S");
			argv[10] = g_build_path( "/", tmp_folder, "resfacebook", NULL );
			argv[11] = g_strdup("-S");
			argv[12] = g_build_path( "/", tmp_folder, "resgoogle", NULL );
			argv[13] = g_strdup("-F");
			argv[14] = g_strdup( output_file );
			argv[15] = g_strdup("--auto-add-overlay");
			argv[16] = NULL;
		}

		if ( !utils_spawn_sync( tmp_folder, argv, NULL, 0, NULL, NULL, NULL, NULL, &status, &error) )
		{
			SHOW_ERR1( "Failed to run packaging tool: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto android_dialog_cleanup2;
		}
		
		if ( status != 0 )
		{
			SHOW_ERR1( "Package tool returned error code: %d", status );
			goto android_dialog_cleanup2;
		}

		while (gtk_events_pending())
			gtk_main_iteration();

		g_rename( output_file, output_file_zip );

		// open APK as a zip file
		if ( !mz_zip_reader_init_file( &zip_archive, output_file_zip, 0 ) )
		{
			SHOW_ERR( "Failed to initialise zip file for reading" );
			goto android_dialog_cleanup2;
		}
		if ( !mz_zip_writer_init_from_reader( &zip_archive, output_file_zip ) )
		{
			SHOW_ERR( "Failed to open zip file for writing" );
			goto android_dialog_cleanup2;
		}

		// copy in extra files
		zip_add_file = g_build_path( "/", src_folder, "classes.dex", NULL );
		mz_zip_writer_add_file( &zip_archive, "classes.dex", zip_add_file, NULL, 0, 9 );
		g_free( zip_add_file );

		zip_add_file = g_build_path( "/", android_folder, "lib", "armeabi", "libandroid_player.so", NULL );
		mz_zip_writer_add_file( &zip_archive, "lib/armeabi/libandroid_player.so", zip_add_file, NULL, 0, 9 );
		g_free( zip_add_file );

		zip_add_file = g_build_path( "/", android_folder, "lib", "armeabi-v7a", "libandroid_player.so", NULL );
		mz_zip_writer_add_file( &zip_archive, "lib/armeabi-v7a/libandroid_player.so", zip_add_file, NULL, 0, 9 );
		g_free( zip_add_file );

		zip_add_file = g_build_path( "/", android_folder, "lib", "x86", "libandroid_player.so", NULL );
		mz_zip_writer_add_file( &zip_archive, "lib/x86/libandroid_player.so", zip_add_file, NULL, 0, 9 );
		g_free( zip_add_file );

		while (gtk_events_pending())
			gtk_main_iteration();

		// copy in media files
		zip_add_file = g_build_path( "/", app->project->base_path, "media", NULL );
		if ( !utils_add_folder_to_zip( &zip_archive, zip_add_file, "assets/media", TRUE, TRUE ) )
		{
			SHOW_ERR( "Failed to add media files to APK" );
			goto android_dialog_cleanup2;
		}

		if ( !mz_zip_writer_finalize_archive( &zip_archive ) )
		{
			SHOW_ERR( "Failed to add finalize zip file" );
			goto android_dialog_cleanup2;
		}
		if ( !mz_zip_writer_end( &zip_archive ) )
		{
			SHOW_ERR( "Failed to end zip file" );
			goto android_dialog_cleanup2;
		}

		while (gtk_events_pending())
			gtk_main_iteration();

		// sign apk
		argv2 = g_new0( gchar*, 14 );
		argv2[0] = g_strdup( path_to_jarsigner );
		argv2[1] = g_strdup("-sigalg");
		argv2[2] = g_strdup("MD5withRSA");
		argv2[3] = g_strdup("-digestalg");
		argv2[4] = g_strdup("SHA1");
		argv2[5] = g_strdup("-storepass");
		argv2[6] = g_strdup(keystore_password);
		argv2[7] = g_strdup("-keystore");
		argv2[8] = g_strdup(keystore_file);
		argv2[9] = g_strdup(output_file_zip);
		argv2[10] = g_strdup(alias_name);
		argv2[11] = g_strdup("-keypass");
		argv2[12] = g_strdup(alias_password);
		argv2[13] = NULL;

		if ( !utils_spawn_sync( tmp_folder, argv2, NULL, 0, NULL, NULL, &str_out, NULL, &status, &error) )
		{
			SHOW_ERR1( "Failed to run signing tool: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto android_dialog_cleanup2;
		}
		
		if ( status != 0 )
		{
			if ( str_out && *str_out ) SHOW_ERR1( "Failed to sign APK, is your keystore password and alias correct? (error: %s)", str_out );
			else SHOW_ERR1( "Failed to sign APK, is your keystore password and alias correct? (error: %d)", status );
			goto android_dialog_cleanup2;
		}

		if ( str_out ) g_free(str_out);
		str_out = 0;

		while (gtk_events_pending())
			gtk_main_iteration();

		// align apk
		argv3 = g_new0( gchar*, 5 );
		argv3[0] = g_strdup( path_to_zipalign );
		argv3[1] = g_strdup("4");
		argv3[2] = g_strdup(output_file_zip);
		argv3[3] = g_strdup(output_file);
		argv3[4] = NULL;

		if ( !utils_spawn_sync( tmp_folder, argv3, NULL, 0, NULL, NULL, &str_out, NULL, &status, &error) )
		{
			SHOW_ERR1( "Failed to run zipalign tool: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto android_dialog_cleanup2;
		}
		
		if ( status != 0 )
		{
			if ( str_out && *str_out ) SHOW_ERR1( "Zip align tool returned error: %s", str_out );
			else SHOW_ERR1( "Zip align tool returned error code: %d", status );
			goto android_dialog_cleanup2;
		}

		while (gtk_events_pending())
			gtk_main_iteration();

		gtk_widget_hide(GTK_WIDGET(dialog));

android_dialog_cleanup2:
        
        gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.android_dialog, "android_export1"), TRUE );
		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.android_dialog, "button7"), TRUE );

		g_unlink( output_file_zip );
		utils_remove_folder_recursive( tmp_folder );

		if ( path_to_aapt ) g_free(path_to_aapt);
		if ( path_to_android_jar ) g_free(path_to_android_jar);
		if ( path_to_jarsigner ) g_free(path_to_jarsigner);

		if ( zip_add_file ) g_free(zip_add_file);
		if ( manifest_file ) g_free(manifest_file);
		if ( contents ) g_free(contents);
		if ( resources_file ) g_free(resources_file);
		if ( error ) g_error_free(error);
		if ( icon_image ) gdk_pixbuf_unref(icon_image);
		if ( image_filename ) g_free(image_filename);
		if ( icon_scaled_image ) gdk_pixbuf_unref(icon_scaled_image);
		if ( argv ) g_strfreev(argv);
		if ( argv2 ) g_strfreev(argv2);
		if ( argv3 ) g_strfreev(argv3);
		
		if ( output_file_zip ) g_free(output_file_zip);
		if ( tmp_folder ) g_free(tmp_folder);
		if ( android_folder ) g_free(android_folder);
		if ( src_folder ) g_free(src_folder);

		if ( output_file ) g_free(output_file);
		if ( app_name ) g_free(app_name);
		if ( package_name ) g_free(package_name);
		if ( app_icon ) g_free(app_icon);
		if ( ouya_icon ) g_free(ouya_icon);

		if ( keystore_file ) g_free(keystore_file);
		if ( keystore_password ) g_free(keystore_password);
		if ( version_number ) g_free(version_number);
		if ( alias_name ) g_free(alias_name);
		if ( alias_password ) g_free(alias_password);
	}

	running = 0;
}

void project_export_apk()
{
	static GeanyProject *last_proj = 0;

	if ( !app->project ) 
	{
		SHOW_ERR( "You must have a project open to export it" );
		return;
	}

	if (ui_widgets.android_dialog == NULL)
	{
		ui_widgets.android_dialog = create_android_dialog();
		gtk_widget_set_name(ui_widgets.android_dialog, "Export APK");
		gtk_window_set_transient_for(GTK_WINDOW(ui_widgets.android_dialog), GTK_WINDOW(main_widgets.window));

		g_signal_connect(ui_widgets.android_dialog, "response", G_CALLBACK(on_android_dialog_response), NULL);
        g_signal_connect(ui_widgets.android_dialog, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);

		ui_setup_open_button_callback_android(ui_lookup_widget(ui_widgets.android_dialog, "android_app_icon_path"), NULL,
			GTK_FILE_CHOOSER_ACTION_OPEN, GTK_ENTRY(ui_lookup_widget(ui_widgets.android_dialog, "android_app_icon_entry")));
		ui_setup_open_button_callback_android(ui_lookup_widget(ui_widgets.android_dialog, "android_ouya_icon_path"), NULL,
			GTK_FILE_CHOOSER_ACTION_OPEN, GTK_ENTRY(ui_lookup_widget(ui_widgets.android_dialog, "android_ouya_icon_entry")));
		ui_setup_open_button_callback_android(ui_lookup_widget(ui_widgets.android_dialog, "android_keystore_file_path"), NULL,
			GTK_FILE_CHOOSER_ACTION_OPEN, GTK_ENTRY(ui_lookup_widget(ui_widgets.android_dialog, "android_keystore_file_entry")));

		ui_setup_open_button_callback_android(ui_lookup_widget(ui_widgets.android_dialog, "android_output_file_path"), NULL,
			GTK_FILE_CHOOSER_ACTION_SAVE, GTK_ENTRY(ui_lookup_widget(ui_widgets.android_dialog, "android_output_file_entry")));

		gtk_combo_box_set_active( GTK_COMBO_BOX(ui_lookup_widget(ui_widgets.android_dialog, "android_output_type_combo")), 0 );
		gtk_combo_box_set_active( GTK_COMBO_BOX(ui_lookup_widget(ui_widgets.android_dialog, "android_orientation_combo")), 0 );
		gtk_combo_box_set_active( GTK_COMBO_BOX(ui_lookup_widget(ui_widgets.android_dialog, "android_sdk_combo")), 0 ); 
	}

	if ( app->project != last_proj )
	{
		last_proj = app->project;
		gchar *filename = g_strconcat( app->project->name, ".apk", NULL );
		gchar* apk_path = g_build_filename( app->project->base_path, filename, NULL );
		gtk_entry_set_text( GTK_ENTRY(ui_lookup_widget(ui_widgets.android_dialog, "android_output_file_entry")), apk_path );
		g_free(apk_path);
		g_free(filename);
	}

	gtk_window_present(GTK_WINDOW(ui_widgets.android_dialog));
}

static void on_keystore_dialog_response(GtkDialog *dialog, gint response, gpointer user_data)
{
	static int running = 0;
	if ( running ) return;

	running = 1;

	if ( response != 1 )
	{
		gtk_widget_hide(GTK_WIDGET(dialog));
	}
	else
	{
		int i;
		GtkWidget *widget;

		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.keystore_dialog, "button9"), FALSE );
		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.keystore_dialog, "button8"), FALSE );
		
		// keystore details
		widget = ui_lookup_widget(ui_widgets.keystore_dialog, "keystore_full_name_entry");
		gchar *full_name = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));
		
		widget = ui_lookup_widget(ui_widgets.keystore_dialog, "keystore_company_name_entry");
		gchar *company_name = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		widget = ui_lookup_widget(ui_widgets.keystore_dialog, "keystore_city_entry");
		gchar *city = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		widget = ui_lookup_widget(ui_widgets.keystore_dialog, "keystore_country_entry");
		gchar *country = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		widget = ui_lookup_widget(ui_widgets.keystore_dialog, "keystore_password1_entry");
		gchar *password1 = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		widget = ui_lookup_widget(ui_widgets.keystore_dialog, "keystore_password2_entry");
		gchar *password2 = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		// output
		widget = ui_lookup_widget(ui_widgets.keystore_dialog, "keystore_output_file_entry");
		gchar *output_file = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		// START CHECKS

		if ( !output_file || !*output_file ) { SHOW_ERR("You must choose an output location to save your keystore file"); goto keystore_dialog_clean_up; }

		if ( g_file_test( output_file, G_FILE_TEST_EXISTS ) )
		{
			if ( !dialogs_show_question(_("\"%s\" already exists. Do you want to overwrite it?"), output_file) )
			{
				goto keystore_dialog_clean_up;
			}
		}

		// check full name
		if ( strlen(full_name) > 30 ) { SHOW_ERR("Full name must be less than 30 characters"); goto keystore_dialog_clean_up; }
		for( i = 0; i < strlen(full_name); i++ )
		{
			if ( (full_name[i] < 97 || full_name[i] > 122)
			  && (full_name[i] < 65 || full_name[i] > 90) 
			  && full_name[i] != 32 ) 
			{ 
				SHOW_ERR("Full name contains invalid characters, must be A-Z and spaces only"); 
				goto keystore_dialog_clean_up; 
			}
		}
		if ( !*full_name )
		{
			g_free(full_name);
			full_name = g_strdup("Unknown");
		}

		// check company name
		if ( strlen(company_name) > 30 ) { SHOW_ERR("Company name must be less than 30 characters"); goto keystore_dialog_clean_up; }
		for( i = 0; i < strlen(company_name); i++ )
		{
			if ( (company_name[i] < 97 || company_name[i] > 122)
			  && (company_name[i] < 65 || company_name[i] > 90) 
			  && company_name[i] != 32 ) 
			{ 
				SHOW_ERR("Company name contains invalid characters, must be A-Z and spaces only"); 
				goto keystore_dialog_clean_up; 
			}
		}
		if ( !*company_name )
		{
			g_free(company_name);
			company_name = g_strdup("Unknown");
		}

		// city
		if ( strlen(city) > 30 ) { SHOW_ERR("City must be less than 30 characters"); goto keystore_dialog_clean_up; }
		for( i = 0; i < strlen(city); i++ )
		{
			if ( (city[i] < 97 || city[i] > 122)
			  && (city[i] < 65 || city[i] > 90) 
			  && city[i] != 32 ) 
			{ 
				SHOW_ERR("City contains invalid characters, must be A-Z and spaces only"); 
				goto keystore_dialog_clean_up; 
			}
		}
		if ( !*city )
		{
			g_free(city);
			city = g_strdup("Unknown");
		}

		// country
		if ( strlen(country) > 0 && strlen(country) != 2 ) { SHOW_ERR("Country code must be 2 characters"); goto keystore_dialog_clean_up; }
		for( i = 0; i < strlen(city); i++ )
		{
			if ( (city[i] < 97 || city[i] > 122)
			  && (city[i] < 65 || city[i] > 90) ) 
			{ 
				SHOW_ERR("Country code contains invalid characters, must be A-Z only"); 
				goto keystore_dialog_clean_up; 
			}
		}
		if ( !*country )
		{
			g_free(country);
			country = g_strdup("Unknown");
		}
		
		// check passwords
		if ( !password1 || !*password1 ) { SHOW_ERR("Password cannot be blank"); goto keystore_dialog_clean_up; }
		if ( strlen(password1) < 6 ) { SHOW_ERR("Password must be at least 6 characters long"); goto keystore_dialog_clean_up; }
		if ( strchr(password1,'"') ) { SHOW_ERR("Password cannot contain double quotes"); goto keystore_dialog_clean_up; }
		if ( strcmp(password1,password2) != 0 ) { SHOW_ERR("Passwords do not match"); goto keystore_dialog_clean_up; }

		goto keystore_dialog_continue;

keystore_dialog_clean_up:
		if ( full_name ) g_free(full_name);
		if ( company_name ) g_free(company_name);
		if ( city ) g_free(city);
		if ( country ) g_free(country);
		if ( password1 ) g_free(password1);
		if ( password2 ) g_free(password2);

		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.keystore_dialog, "button8"), TRUE );
		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.keystore_dialog, "button9"), TRUE );
		running = 0;
		return;

keystore_dialog_continue:

		;

		// CHECKS COMPLETE, START KEY GENERATION

#ifdef G_OS_WIN32
		gchar* path_to_keytool = g_build_path( "/", app->datadir, "android", "jre", "bin", "keytool.exe", NULL );
#else
        //gchar* path_to_jarsigner = g_build_path( "/", "/usr", "bin", "keytool", NULL );
        gchar* path_to_keytool = g_build_path( "/", app->datadir, "android", "jre", "bin", "keytool", NULL );
#endif

		// decalrations
		gchar **argv = NULL;
		gchar *dname = NULL;
		int status = 0;
		GError *error = 0;
		gchar *keystore_name = NULL;
		gchar* str_out = 0;

		utils_str_replace_char( output_file, '\\', '/' );
		gchar* slash = strrchr( output_file, '/' );
		if ( slash )
		{
			keystore_name = g_strdup( slash+1 );
			*slash = 0;
		}
		else
		{
			keystore_name = g_strdup( output_file );
			g_free(output_file);
			output_file = local_prefs.project_file_path;
		}

		if ( !g_file_test( path_to_keytool, G_FILE_TEST_EXISTS ) )
		{
			SHOW_ERR1( "Could not find keytool program, the path \"%s\" is incorrect", path_to_keytool );
			goto keystore_dialog_cleanup2;
		}

		dname = g_strdup_printf( "CN=%s, O=%s, L=%s, C=%s", full_name, company_name, city, country );
		
		// package manifest and resources
		argv = g_new0( gchar*, 19 );
		argv[0] = g_strdup( path_to_keytool );
		argv[1] = g_strdup("-genkey");
		argv[2] = g_strdup("-keystore");
		argv[3] = g_strdup(keystore_name);
		argv[4] = g_strdup("-alias");
		argv[5] = g_strdup("mykeystore");
		argv[6] = g_strdup("-keyalg");
		argv[7] = g_strdup("RSA");
		argv[8] = g_strdup("-keysize");
		argv[9] = g_strdup("2048");
		argv[10] = g_strdup("-validity");
		argv[11] = g_strdup("10000");
		argv[12] = g_strdup("-storepass");
		argv[13] = g_strdup(password1);
		argv[14] = g_strdup("-keypass");
		argv[15] = g_strdup(password1);
		argv[16] = g_strdup("-dname");
		argv[17] = g_strdup(dname);
		argv[18] = NULL;

		if ( !utils_spawn_sync( output_file, argv, NULL, 0, NULL, NULL, &str_out, NULL, &status, &error) )
		{
			SHOW_ERR1( "Failed to run keytool program: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto keystore_dialog_cleanup2;
		}
		
		if ( status != 0 )
		{
			if ( str_out && *str_out ) SHOW_ERR1( "keytool program returned error: %s", str_out );
			else SHOW_ERR1( "keytool program returned error code: %d", status );
			goto keystore_dialog_cleanup2;
		}

		gtk_widget_hide(GTK_WIDGET(dialog));

keystore_dialog_cleanup2:
        
        gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.keystore_dialog, "button8"), TRUE );
		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.keystore_dialog, "button9"), TRUE );

		if ( error ) g_error_free(error);

		if ( path_to_keytool ) g_free(path_to_keytool);
		if ( argv ) g_strfreev(argv);
		if ( dname ) g_free(dname);
		if ( keystore_name ) g_free(keystore_name);
		if ( str_out ) g_free(str_out);
		
		if ( full_name ) g_free(full_name);
		if ( company_name ) g_free(company_name);
		if ( city ) g_free(city);
		if ( country ) g_free(country);
		if ( password1 ) g_free(password1);
		if ( password2 ) g_free(password2);
	}

	running = 0;
}

void project_generate_keystore()
{

	if (ui_widgets.keystore_dialog == NULL)
	{
		ui_widgets.keystore_dialog = create_keystore_dialog();
		gtk_widget_set_name(ui_widgets.keystore_dialog, "Generate Keystore");
		gtk_window_set_transient_for(GTK_WINDOW(ui_widgets.keystore_dialog), GTK_WINDOW(main_widgets.window));

		g_signal_connect(ui_widgets.keystore_dialog, "response", G_CALLBACK(on_keystore_dialog_response), NULL);
        g_signal_connect(ui_widgets.keystore_dialog, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);

		ui_setup_open_button_callback_keystore(ui_lookup_widget(ui_widgets.keystore_dialog, "keystore_output_file_path"), NULL,
			GTK_FILE_CHOOSER_ACTION_SAVE, GTK_ENTRY(ui_lookup_widget(ui_widgets.keystore_dialog, "keystore_output_file_entry"))); 
	}

	GtkWidget *widget = ui_lookup_widget(ui_widgets.keystore_dialog, "keystore_output_file_entry");
	const gchar *output_file = gtk_entry_get_text(GTK_ENTRY(widget));
	if ( !output_file || !*output_file )
	{
		gchar* out_path = g_build_filename( local_prefs.project_file_path, "release.keystore", NULL );
		gtk_entry_set_text( GTK_ENTRY(ui_lookup_widget(ui_widgets.keystore_dialog, "keystore_output_file_entry")), out_path );
		g_free(out_path);
	}

	gtk_window_present(GTK_WINDOW(ui_widgets.keystore_dialog));
}

static void on_ios_dialog_response(GtkDialog *dialog, gint response, gpointer user_data)
{
	static int running = 0;
	if ( running ) return;

	running = 1;

	if ( response != 1 )
	{
		gtk_widget_hide(GTK_WIDGET(dialog));
	}
	else
	{
		int i;
		GtkWidget *widget;

		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.ios_dialog, "ios_export1"), FALSE );
		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.ios_dialog, "button6"), FALSE );
		
		while (gtk_events_pending())
			gtk_main_iteration();

		// app details
		widget = ui_lookup_widget(ui_widgets.ios_dialog, "ios_app_name_entry");
		gchar *app_name = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));
		
		widget = ui_lookup_widget(ui_widgets.ios_dialog, "ios_provisioning_entry");
		gchar *profile = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		widget = ui_lookup_widget(ui_widgets.ios_dialog, "ios_app_icon_entry");
		gchar *app_icon = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		widget = ui_lookup_widget(ui_widgets.ios_dialog, "ios_facebook_id_entry");
		gchar *facebook_id = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));

		widget = ui_lookup_widget(ui_widgets.ios_dialog, "ios_orientation_combo");
		gchar *app_orientation = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(widget));
		int orientation = 0;
		if ( strcmp(app_orientation,"Landscape Left") == 0 ) orientation = 0;
		else if ( strcmp(app_orientation,"Landscape Right") == 0 ) orientation = 1;
		else if ( strcmp(app_orientation,"Portrait") == 0 ) orientation = 2;
		else if ( strcmp(app_orientation,"Portrait Upside Down") == 0 ) orientation = 3;
		g_free(app_orientation);
		
		widget = ui_lookup_widget(ui_widgets.ios_dialog, "ios_version_number_entry");
		gchar *version_number = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));
		if ( !*version_number ) SETPTR( version_number, g_strdup("1.0.0") );

		// output
		widget = ui_lookup_widget(ui_widgets.ios_dialog, "ios_output_file_entry");
		gchar *output_file = g_strdup(gtk_entry_get_text(GTK_ENTRY(widget)));


		// START CHECKS

		if ( !output_file || !*output_file ) { SHOW_ERR("You must choose an output location to save your IPA"); goto ios_dialog_clean_up; }

		// check app name
		if ( !app_name || !*app_name ) { SHOW_ERR("You must enter an app name"); goto ios_dialog_clean_up; }
		if ( strlen(app_name) > 30 ) { SHOW_ERR("App name must be less than 30 characters"); goto ios_dialog_clean_up; }
		for( i = 0; i < strlen(app_name); i++ )
		{
			if ( (app_name[i] < 97 || app_name[i] > 122)
			  && (app_name[i] < 65 || app_name[i] > 90) 
			  && (app_name[i] < 48 || app_name[i] > 57) 
			  && app_name[i] != 32 
			  && app_name[i] != 95 ) 
			{ 
				SHOW_ERR("App name contains invalid characters, must be A-Z 0-9 spaces and undersore only"); 
				goto ios_dialog_clean_up; 
			}
		}
		
		// check icon
		//if ( !app_icon || !*app_icon ) { SHOW_ERR("You must select an app icon"); goto ios_dialog_clean_up; }
		if ( app_icon && *app_icon )
		{
			if ( !strrchr( app_icon, '.' ) || utils_str_casecmp( strrchr( app_icon, '.' ), ".png" ) != 0 ) { SHOW_ERR("App icon must be a PNG file"); goto ios_dialog_clean_up; }
			if ( !g_file_test( app_icon, G_FILE_TEST_EXISTS ) ) { SHOW_ERR("Could not find app icon location"); goto ios_dialog_clean_up; }
		}

		// check profile
		if ( !profile || !*profile ) { SHOW_ERR("You must select a provisioning profile"); goto ios_dialog_clean_up; }
		if ( !strrchr( profile, '.' ) || utils_str_casecmp( strrchr( profile, '.' ), ".mobileprovision" ) != 0 ) { SHOW_ERR("Provisioning profile must have .mobileprovision extension"); goto ios_dialog_clean_up; }
		if ( !g_file_test( profile, G_FILE_TEST_EXISTS ) ) { SHOW_ERR("Could not find provisioning profile location"); goto ios_dialog_clean_up; }

		// check version
		if ( !version_number || !*version_number ) { SHOW_ERR("You must enter a version number, e.g. 1.0.0"); goto ios_dialog_clean_up; }
		for( i = 0; i < strlen(version_number); i++ )
		{
			if ( (version_number[i] < 48 || version_number[i] > 57) && version_number[i] != 46 ) 
			{ 
				SHOW_ERR("Version number contains invalid characters, must be 0-9 and . only"); 
				goto ios_dialog_clean_up; 
			}
		}

		// check facebook id
		if ( facebook_id && *facebook_id )
		{
			for( i = 0; i < strlen(facebook_id); i++ )
			{
				if ( (facebook_id[i] < 48 || facebook_id[i] > 57) ) 
				{ 
					SHOW_ERR("Facebook App ID must be numbers only"); 
					goto ios_dialog_clean_up; 
				}
			}
		}
	
		goto ios_dialog_continue;

ios_dialog_clean_up:
		if ( app_name ) g_free(app_name);
		if ( profile ) g_free(profile);
		if ( app_icon ) g_free(app_icon);
		if ( facebook_id ) g_free(facebook_id);
		if ( version_number ) g_free(version_number);
		if ( output_file ) g_free(output_file);

		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.ios_dialog, "ios_export1"), TRUE );
		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.ios_dialog, "button6"), TRUE );
		running = 0;
		return;

ios_dialog_continue:

		while (gtk_events_pending())
			gtk_main_iteration();

		// CHECKS COMPLETE, START EXPORT

		gchar* path_to_codesign = g_strdup("/usr/bin/codesign");
		gchar* path_to_security = g_strdup("/usr/bin/security");

		// make temporary folder
		gchar* ios_folder = g_build_filename( app->datadir, "ios", NULL );
		gchar* tmp_folder;
        if ( app->project )
            tmp_folder = g_build_filename( app->project->base_path, "build_tmp", NULL );
        else
            tmp_folder = g_build_filename( local_prefs.project_file_path, "build_tmp", NULL );
		
        gchar* app_folder = g_build_filename( tmp_folder, app_name, NULL );
		SETPTR(app_folder, g_strconcat( app_folder, ".app", NULL ));

		utils_str_replace_char( ios_folder, '\\', '/' );
		utils_str_replace_char( tmp_folder, '\\', '/' );
		
		gchar* src_folder = g_build_path( "/", app->datadir, "ios", "source", "AGK 2 Player.app", NULL );
		utils_str_replace_char( src_folder, '\\', '/' );

		gchar *output_file_zip = g_strdup( output_file );
		gchar *ext = strrchr( output_file_zip, '.' );
		if ( ext ) *ext = 0;
		SETPTR( output_file_zip, g_strconcat( output_file_zip, ".zip", NULL ) );

		// decalrations
		gchar newcontents[ 32000 ];
		gchar *contents = NULL;
		gsize length = 0;
		gchar *certificate_data = NULL;
		gchar *bundle_id = NULL;
		gchar *team_id = NULL;
		gchar *cert_hash = NULL;
		gchar *cert_temp = NULL;
		gchar **argv = NULL;
		gchar *str_out = NULL;
		gint status = 0;
		GError *error = NULL;
		gchar *entitlements_file = NULL;
		gchar *temp_filename1 = NULL;
		gchar *temp_filename2 = NULL;
		gchar *version_string = NULL;
		gchar *bundle_id2 = NULL; // don't free, pointer to sub string
		gchar *image_filename = NULL;
		GdkPixbuf *icon_scaled_image = NULL;
		GdkPixbuf *icon_image = NULL;
		gchar *user_name = NULL;
		gchar *group_name = NULL;
		mz_zip_archive zip_archive;
		memset(&zip_archive, 0, sizeof(zip_archive));
		
		if ( !utils_copy_folder( src_folder, app_folder, TRUE ) )
		{
			SHOW_ERR( "Failed to copy source folder" );
			goto ios_dialog_cleanup2;
		}

		// rename executable
		g_chdir( app_folder );
		g_rename( "AGK 2 Player", app_name );

		while (gtk_events_pending())
			gtk_main_iteration();

		// open provisioning profile and extract certificate
		if ( !g_file_get_contents( profile, &contents, &length, NULL ) )
		{
			SHOW_ERR( "Failed to read provisioning profile" );
			goto ios_dialog_cleanup2;
		}

        // provisioning profile starts as binary, so skip 100 bytes to get to text
		gchar* certificate = strstr( contents+100, "<key>DeveloperCertificates</key>" );
		if ( !certificate )
		{
			SHOW_ERR( "Failed to read certificate from provisioning profile" );
			goto ios_dialog_cleanup2;
		}

		certificate = strstr( certificate, "<data>" );
		if ( !certificate )
		{
			SHOW_ERR( "Failed to read certificate data from provisioning profile" );
			goto ios_dialog_cleanup2;
		}

		certificate += strlen("<data>");
		gchar* certificate_end = strstr( certificate, "</data>" );
		if ( !certificate_end )
		{
			SHOW_ERR( "Failed to read certificate end data from provisioning profile" );
			goto ios_dialog_cleanup2;
		}

        // copy certificate into local storage
		gint cert_length = (gint) (certificate_end - certificate);
		certificate_data = g_new0( gchar, cert_length+1 );
		strncpy( certificate_data, certificate, cert_length );
		certificate_data[ cert_length ] = 0;

		utils_str_remove_chars( certificate_data, "\n\r" );
        
		// extract bundle ID, reuse variables
		certificate = strstr( contents+100, "<key>application-identifier</key>" );
		if ( !certificate )
		{
			SHOW_ERR( "Failed to read bundle ID from provisioning profile" );
			goto ios_dialog_cleanup2;
		}

		certificate = strstr( certificate, "<string>" );
		if ( !certificate )
		{
			SHOW_ERR( "Failed to read bundle ID data from provisioning profile" );
			goto ios_dialog_cleanup2;
		}

		certificate += strlen("<string>");
		certificate_end = strstr( certificate, "</string>" );
		if ( !certificate_end )
		{
			SHOW_ERR( "Failed to read bundle ID end data from provisioning profile" );
			goto ios_dialog_cleanup2;
		}
		
        // copy bundle ID to local storage
		cert_length = (gint) (certificate_end - certificate);
		bundle_id = g_new0( gchar, cert_length+1 );
		strncpy( bundle_id, certificate, cert_length );
		bundle_id[ cert_length ] = 0;
		
		// extract team ID, reuse variables
		certificate = strstr( contents+100, "<key>com.apple.developer.team-identifier</key>" );
		if ( !certificate )
		{
			SHOW_ERR( "Failed to read team ID from provisioning profile" );
			goto ios_dialog_cleanup2;
		}

		certificate = strstr( certificate, "<string>" );
		if ( !certificate )
		{
			SHOW_ERR( "Failed to read team ID data from provisioning profile" );
			goto ios_dialog_cleanup2;
		}

		certificate += strlen("<string>");
		certificate_end = strstr( certificate, "</string>" );
		if ( !certificate_end )
		{
			SHOW_ERR( "Failed to read team ID end data from provisioning profile" );
			goto ios_dialog_cleanup2;
		}
		
        // copy team ID to local storage
		cert_length = (gint) (certificate_end - certificate);
		team_id = g_new0( gchar, cert_length+1 );
		strncpy( team_id, certificate, cert_length );
		team_id[ cert_length ] = 0;
		
		if ( strncmp( team_id, bundle_id, strlen(team_id) ) == 0 )
		{
			// remove team ID
			bundle_id2 = strchr( bundle_id, '.' );
			if ( bundle_id2 ) bundle_id2++;
			else bundle_id2 = bundle_id;
		}
		else
		{
			bundle_id2 = bundle_id;
		}

		// find all certificates, the identity is just the hash of the certificate
		argv = g_new0( gchar*, 8 );
		argv[0] = g_strdup( path_to_security );
		argv[1] = g_strdup("find-certificate");
		argv[2] = g_strdup("-a");
		argv[3] = g_strdup("-c");
		argv[4] = g_strdup("iPhone");
		argv[5] = g_strdup("-p"); // use PEM format, same as provisioning profile
		argv[6] = g_strdup("-Z"); // display hash
		argv[7] = NULL;

		if ( !utils_spawn_sync( tmp_folder, argv, NULL, 0, NULL, NULL, &str_out, NULL, &status, &error) )
		{
			SHOW_ERR1( "Failed to run \"security\" program: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto ios_dialog_cleanup2;
		}
		
		if ( status != 0 || !str_out )
		{
			if ( str_out && *str_out ) SHOW_ERR1( "Failed to get code signing identities (error: %s)", str_out );
			else SHOW_ERR1( "Failed to get code signing identities (error: %d)", status );
			goto ios_dialog_cleanup2;
		}

        // cycle through each certificate looking for one that matches provisioning profile
		gchar *sha = strstr( str_out, "SHA-1 hash: " );
		while ( sha )
		{
			sha += strlen( "SHA-1 hash: " );
			gchar *sha_end = strchr( sha, '\n' );
			if ( !sha_end )
			{
				SHOW_ERR( "Failed to read code signing identity from certificate list" );
				goto ios_dialog_cleanup2;
			}

			gint length = (gint) (sha_end - sha);

            // save hash for later, if this is the correct certificate then this will be the codesigning identity
			if ( cert_hash ) g_free(cert_hash);
			cert_hash = g_new0( gchar, length+1 );
			strncpy( cert_hash, sha, length );
			cert_hash[ length ] = 0;

			sha = sha_end + 1;
			sha = strstr( sha, "-----BEGIN CERTIFICATE-----" );
			if ( !sha )
			{
				SHOW_ERR( "Failed to read certificate data from certificate list" );
				goto ios_dialog_cleanup2;
			}

			sha += strlen( "-----BEGIN CERTIFICATE-----" ) + 1;
			sha_end = strstr( sha, "-----END CERTIFICATE-----" );
			if ( !sha_end )
			{
				SHOW_ERR( "Failed to read certificate end data from certificate list" );
				goto ios_dialog_cleanup2;
			}

			length = (gint) (sha_end - sha);

            // copy certificate to temp variable and check it
			if ( cert_temp ) g_free(cert_temp);
			cert_temp = g_new0( gchar, length+1 );
			strncpy( cert_temp, sha, length );
			cert_temp[ length ] = 0;

            // remove new line characters
			utils_str_remove_chars( cert_temp, "\n\r" );
            
			if ( strcmp( cert_temp, certificate_data ) == 0 ) break; // we found the certificate

			if ( cert_hash ) g_free(cert_hash);
			cert_hash = 0;

            // look for next certificate
			sha = sha_end+1;
			sha = strstr( sha, "SHA-1 hash: " );
		}

		if ( !cert_hash )
		{
			SHOW_ERR( "Could not find the certificate used to create the provisioning profile, have you added the certificate to your keychain?" );
			goto ios_dialog_cleanup2;
		}

		g_free(str_out);
		str_out = 0;

		g_strfreev(argv);

		// find all valid identities
		argv = g_new0( gchar*, 6 );
		argv[0] = g_strdup( path_to_security );
		argv[1] = g_strdup("find-identity");
		argv[2] = g_strdup("-p");
		argv[3] = g_strdup("codesigning");
		argv[4] = g_strdup("-v");
		argv[5] = NULL;

		if ( !utils_spawn_sync( tmp_folder, argv, NULL, 0, NULL, NULL, &str_out, NULL, &status, &error) )
		{
			SHOW_ERR1( "Failed to run \"security\" program: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto ios_dialog_cleanup2;
		}
		
		if ( status != 0 || !str_out )
		{
			if ( str_out && *str_out ) SHOW_ERR1( "Failed to get code signing identities (error: %s)", str_out );
			else SHOW_ERR1( "Failed to get code signing identities (error: %d)", status );
			goto ios_dialog_cleanup2;
		}

		// parse identities, look for the identity we found earlier
		if ( strstr( str_out, cert_hash ) == 0 )
		{
			SHOW_ERR( "Signing certificate is not valid, either the private key is missing from your keychain, or the certificate has expired" );
			goto ios_dialog_cleanup2;
		}
		
		if ( str_out ) g_free(str_out);
		str_out = 0;

		while (gtk_events_pending())
			gtk_main_iteration();

		// write entitlements file
		strcpy( newcontents, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n\
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n\
<plist version=\"1.0\">\n<dict>\n	<key>application-identifier</key>\n	<string>" );
		strcat( newcontents, bundle_id );
		strcat( newcontents, "</string>\n	<key>com.apple.developer.team-identifier</key>\n	<string>" );
		strcat( newcontents, team_id );
		strcat( newcontents, "</string>\n	<key>keychain-access-groups</key>\n	<array>\n		<string>" );
		strcat( newcontents, bundle_id );
		strcat( newcontents, "</string>\n	</array>\n</dict>\n</plist>" );

		entitlements_file = g_build_filename( tmp_folder, "entitlements.xcent", NULL );

		if ( !g_file_set_contents( entitlements_file, newcontents, strlen(newcontents), &error ) )
		{
			SHOW_ERR1( "Failed to write entitlements file: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto ios_dialog_cleanup2;
		}

		// copy provisioning profile
		temp_filename1 = g_build_filename( app_folder, "embedded.mobileprovision", NULL );
		utils_copy_file( profile, temp_filename1, TRUE );

		// edit Info.plist
		g_free(temp_filename1);
		temp_filename1 = g_build_filename( app_folder, "Info.plist", NULL );

		if ( contents ) g_free(contents);
		contents = 0;
		if ( !g_file_get_contents( temp_filename1, &contents, &length, NULL ) )
		{
			SHOW_ERR( "Failed to read Info.plist file" );
			goto ios_dialog_cleanup2;
		}

		utils_str_replace_all( &contents, "${PRODUCT_NAME}", app_name );
		utils_str_replace_all( &contents, "${EXECUTABLE_NAME}", app_name );
		utils_str_replace_all( &contents, "com.thegamecreators.agk2player", bundle_id2 );
		if ( facebook_id && *facebook_id ) utils_str_replace_all( &contents, "358083327620324", facebook_id );
		switch( orientation )
		{
			case 0: utils_str_replace_all( &contents, "UIInterfaceOrientationPortrait", "UIInterfaceOrientationLandscapeLeft" ); break;
			case 1: utils_str_replace_all( &contents, "UIInterfaceOrientationPortrait", "UIInterfaceOrientationLandscapeRight" ); break;
			case 2: utils_str_replace_all( &contents, "UIInterfaceOrientationPortrait", "UIInterfaceOrientationPortrait" ); break;
			case 3: utils_str_replace_all( &contents, "UIInterfaceOrientationPortrait", "UIInterfaceOrientationPortraitUpsideDown" ); break;
		}
		version_string = g_strconcat( "<string>", version_number, "</string>", NULL );
        utils_str_replace_all( &contents, "<string>1.0.0</string>", version_string );
		utils_str_replace_all( &contents, "<string>1.0</string>", version_string );

		if ( !g_file_set_contents( temp_filename1, contents, strlen(contents), NULL ) )
		{
			SHOW_ERR( "Failed to write Info.plist file" );
			goto ios_dialog_cleanup2;
		}

		g_free(str_out);
		str_out = 0;
		g_strfreev(argv);

		// convert plist to binary
		argv = g_new0( gchar*, 6 );
		argv[0] = g_strdup( "/usr/bin/plutil" );
		argv[1] = g_strdup("-convert");
		argv[2] = g_strdup("binary1");
		argv[3] = g_strdup(temp_filename1);
		argv[4] = NULL;

		if ( !utils_spawn_sync( tmp_folder, argv, NULL, 0, NULL, NULL, &str_out, NULL, &status, &error) )
		{
			SHOW_ERR1( "Failed to run userid program: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto ios_dialog_cleanup2;
		}
		
		if ( status != 0 || !str_out )
		{
			if ( str_out && *str_out ) SHOW_ERR1( "Failed to get user name (error: %s)", str_out );
			else SHOW_ERR1( "Failed to get user name (error: %d)", status );
			goto ios_dialog_cleanup2;
		}

		// load icon file
		if ( app_icon && *app_icon )
		{
			icon_image = gdk_pixbuf_new_from_file( app_icon, &error );
			if ( !icon_image || error )
			{
				SHOW_ERR1( "Failed to load image icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto ios_dialog_cleanup2;
			}

			// scale it and save it
			// 152x152
			image_filename = g_build_path( "/", app_folder, "icon-152.png", NULL );
			icon_scaled_image = gdk_pixbuf_scale_simple( icon_image, 152, 152, GDK_INTERP_HYPER );
			if ( !gdk_pixbuf_save( icon_scaled_image, image_filename, "png", &error, "compression", "9", NULL ) )
			{
				SHOW_ERR1( "Failed to save 152x152 icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto ios_dialog_cleanup2;
			}
			gdk_pixbuf_unref( icon_scaled_image );
			g_free( image_filename );

			// 144x144
			image_filename = g_build_path( "/", app_folder, "icon-144.png", NULL );
			icon_scaled_image = gdk_pixbuf_scale_simple( icon_image, 144, 144, GDK_INTERP_HYPER );
			if ( !gdk_pixbuf_save( icon_scaled_image, image_filename, "png", &error, "compression", "9", NULL ) )
			{
				SHOW_ERR1( "Failed to save 144x144 icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto ios_dialog_cleanup2;
			}
			gdk_pixbuf_unref( icon_scaled_image );
			g_free( image_filename );

			// 120x120
			image_filename = g_build_path( "/", app_folder, "icon-120.png", NULL );
			icon_scaled_image = gdk_pixbuf_scale_simple( icon_image, 120, 120, GDK_INTERP_HYPER );
			if ( !gdk_pixbuf_save( icon_scaled_image, image_filename, "png", &error, "compression", "9", NULL ) )
			{
				SHOW_ERR1( "Failed to save 120x120 icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto ios_dialog_cleanup2;
			}
			gdk_pixbuf_unref( icon_scaled_image );
			g_free( image_filename );

			// 114x114
			image_filename = g_build_path( "/", app_folder, "icon-114.png", NULL );
			icon_scaled_image = gdk_pixbuf_scale_simple( icon_image, 114, 114, GDK_INTERP_HYPER );
			if ( !gdk_pixbuf_save( icon_scaled_image, image_filename, "png", &error, "compression", "9", NULL ) )
			{
				SHOW_ERR1( "Failed to save 114x114 icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto ios_dialog_cleanup2;
			}
			gdk_pixbuf_unref( icon_scaled_image );
			g_free( image_filename );

			// 76x76
			image_filename = g_build_path( "/", app_folder, "icon-76.png", NULL );
			icon_scaled_image = gdk_pixbuf_scale_simple( icon_image, 76, 76, GDK_INTERP_HYPER );
			if ( !gdk_pixbuf_save( icon_scaled_image, image_filename, "png", &error, "compression", "9", NULL ) )
			{
				SHOW_ERR1( "Failed to save 76x76 icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto ios_dialog_cleanup2;
			}
			gdk_pixbuf_unref( icon_scaled_image );
			g_free( image_filename );

			// 72x72
			image_filename = g_build_path( "/", app_folder, "icon-72.png", NULL );
			icon_scaled_image = gdk_pixbuf_scale_simple( icon_image, 72, 72, GDK_INTERP_HYPER );
			if ( !gdk_pixbuf_save( icon_scaled_image, image_filename, "png", &error, "compression", "9", NULL ) )
			{
				SHOW_ERR1( "Failed to save 72x72 icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto ios_dialog_cleanup2;
			}
			gdk_pixbuf_unref( icon_scaled_image );
			g_free( image_filename );

			// 60x60
			image_filename = g_build_path( "/", app_folder, "icon-60.png", NULL );
			icon_scaled_image = gdk_pixbuf_scale_simple( icon_image, 60, 60, GDK_INTERP_HYPER );
			if ( !gdk_pixbuf_save( icon_scaled_image, image_filename, "png", &error, "compression", "9", NULL ) )
			{
				SHOW_ERR1( "Failed to save 60x60 icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto ios_dialog_cleanup2;
			}
			gdk_pixbuf_unref( icon_scaled_image );
			g_free( image_filename );

			// 57x57
			image_filename = g_build_path( "/", app_folder, "icon-57.png", NULL );
			icon_scaled_image = gdk_pixbuf_scale_simple( icon_image, 57, 57, GDK_INTERP_HYPER );
			if ( !gdk_pixbuf_save( icon_scaled_image, image_filename, "png", &error, "compression", "9", NULL ) )
			{
				SHOW_ERR1( "Failed to save 57x57 icon: %s", error->message );
				g_error_free(error);
				error = NULL;
				goto ios_dialog_cleanup2;
			}
							
			gdk_pixbuf_unref( icon_scaled_image );
			icon_scaled_image = NULL;

			g_free( image_filename );
			image_filename = NULL;
		}

		while (gtk_events_pending())
			gtk_main_iteration();

		// copy media folder
        if ( app->project )
        {
            if ( temp_filename1 ) g_free(temp_filename1);
            temp_filename1 = g_build_filename( app->project->base_path, "media", NULL );
            temp_filename2 = g_build_filename( app_folder, "media", NULL );
            utils_copy_folder( temp_filename1, temp_filename2, TRUE );
        }

		g_free(str_out);
		str_out = 0;
		g_strfreev(argv);

		// find user name
		argv = g_new0( gchar*, 6 );
		argv[0] = g_strdup( "/usr/bin/id" );
		argv[1] = g_strdup("-u");
		argv[2] = g_strdup("-n");
		argv[3] = NULL;

		if ( !utils_spawn_sync( tmp_folder, argv, NULL, 0, NULL, NULL, &str_out, NULL, &status, &error) )
		{
			SHOW_ERR1( "Failed to run userid program: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto ios_dialog_cleanup2;
		}
		
		if ( status != 0 || !str_out )
		{
			if ( str_out && *str_out ) SHOW_ERR1( "Failed to get user name (error: %s)", str_out );
			else SHOW_ERR1( "Failed to get user name (error: %d)", status );
			goto ios_dialog_cleanup2;
		}

		user_name = g_strdup( str_out );
        user_name[ strlen(user_name)-1 ] = 0;

		g_free(str_out);
		str_out = 0;
		g_strfreev(argv);

		// find group name
		argv = g_new0( gchar*, 6 );
		argv[0] = g_strdup( "/usr/bin/id" );
		argv[1] = g_strdup("-g");
		argv[2] = g_strdup("-n");
		argv[3] = NULL;

		if ( !utils_spawn_sync( tmp_folder, argv, NULL, 0, NULL, NULL, &str_out, NULL, &status, &error) )
		{
			SHOW_ERR1( "Failed to run groupid program: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto ios_dialog_cleanup2;
		}
		
		if ( status != 0 || !str_out )
		{
			if ( str_out && *str_out ) SHOW_ERR1( "Failed to get group name (error: %s)", str_out );
			else SHOW_ERR1( "Failed to get group name (error: %d)", status );
			goto ios_dialog_cleanup2;
		}

		group_name = g_strdup( str_out );
        group_name[ strlen(group_name)-1 ] = 0;

		g_free(str_out);
		str_out = 0;
		g_strfreev(argv);

		// prepare bundle
		argv = g_new0( gchar*, 6 );
		argv[0] = g_strdup( "/usr/sbin/chown" );
		argv[1] = g_strdup("-RH");
		argv[2] = g_strconcat( user_name, ":", group_name, NULL );
		argv[3] = g_strdup(app_folder);
		argv[4] = NULL;
        
		if ( !utils_spawn_sync( tmp_folder, argv, NULL, 0, NULL, NULL, &str_out, NULL, &status, &error) )
		{
			SHOW_ERR1( "Failed to run chown program: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto ios_dialog_cleanup2;
		}
		
		if ( status != 0 )
		{
			if ( str_out && *str_out ) SHOW_ERR1( "Failed to set file ownership (error: %s)", str_out );
			else SHOW_ERR1( "Failed to set file ownership (error: %d)", status );
			goto ios_dialog_cleanup2;
		}

		g_free(str_out);
		str_out = 0;
		g_strfreev(argv);

		// prepare bundle 2
		argv = g_new0( gchar*, 6 );
		argv[0] = g_strdup( "/bin/chmod" );
		argv[1] = g_strdup("-RH");
		argv[2] = g_strdup("u+w,go-w,a+rX");
		argv[3] = g_strdup(app_folder);
		argv[4] = NULL;

		if ( !utils_spawn_sync( tmp_folder, argv, NULL, 0, NULL, NULL, &str_out, NULL, &status, &error) )
		{
			SHOW_ERR1( "Failed to run chmod program: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto ios_dialog_cleanup2;
		}
		
		if ( status != 0 )
		{
			if ( str_out && *str_out ) SHOW_ERR1( "Failed to set file permissions (error: %s)", str_out );
			else SHOW_ERR1( "Failed to set file permissions (error: %d)", status );
			goto ios_dialog_cleanup2;
		}

		g_free(str_out);
		str_out = 0;
		g_strfreev(argv);

		// sign bundle
		argv = g_new0( gchar*, 10 );
		argv[0] = g_strdup( path_to_codesign );
		argv[1] = g_strdup("--force");
		argv[2] = g_strdup("--sign");
		argv[3] = g_strdup(cert_hash);
		argv[4] = g_strdup("--resource-rules");
		argv[5] = g_strconcat( app_folder, "/ResourceRules.plist", NULL );
		argv[6] = g_strdup("--entitlements");
		argv[7] = g_strdup(entitlements_file);
		argv[8] = g_strdup(app_folder);
		argv[9] = NULL;
        
		if ( !utils_spawn_sync( tmp_folder, argv, NULL, 0, NULL, NULL, &str_out, NULL, &status, &error) )
		{
			SHOW_ERR1( "Failed to run codesign program: %s", error->message );
			g_error_free(error);
			error = NULL;
			goto ios_dialog_cleanup2;
		}
		
		if ( status != 0 )
		{
			if ( str_out && *str_out ) SHOW_ERR1( "Failed to sign app (error: %s)", str_out );
			else SHOW_ERR1( "Failed to sign app (error: %d)", status );
			goto ios_dialog_cleanup2;
		}

		// create IPA zip file
		if ( !mz_zip_writer_init_file( &zip_archive, output_file_zip, 0 ) )
		{
			SHOW_ERR( "Failed to initialise zip file for writing" );
			goto ios_dialog_cleanup2;
		}
		
		if ( temp_filename1 ) g_free(temp_filename1);
		temp_filename1 = g_strconcat( "Payload/", app_name, ".app", NULL );
		if ( !utils_add_folder_to_zip( &zip_archive, app_folder, temp_filename1, TRUE, FALSE ) )
		{
			SHOW_ERR( "Failed to add files to IPA" );
			goto ios_dialog_cleanup2;
		}

		if ( !mz_zip_writer_finalize_archive( &zip_archive ) )
		{
			SHOW_ERR( "Failed to finalize IPA file" );
			goto ios_dialog_cleanup2;
		}
		if ( !mz_zip_writer_end( &zip_archive ) )
		{
			SHOW_ERR( "Failed to end IPA file" );
			goto ios_dialog_cleanup2;
		}

		g_rename( output_file_zip, output_file );

		while (gtk_events_pending())
			gtk_main_iteration();

		gtk_widget_hide(GTK_WIDGET(dialog));

ios_dialog_cleanup2:
        
        gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.ios_dialog, "ios_export1"), TRUE );
		gtk_widget_set_sensitive( ui_lookup_widget(ui_widgets.ios_dialog, "button6"), TRUE );

		utils_remove_folder_recursive( tmp_folder );

		if ( path_to_codesign ) g_free(path_to_codesign);
		if ( path_to_security ) g_free(path_to_security);

		if ( output_file_zip ) g_free(output_file_zip);
		if ( ios_folder ) g_free(ios_folder);
		if ( tmp_folder ) g_free(tmp_folder);
		if ( src_folder ) g_free(src_folder);

		if ( error ) g_error_free(error);
		if ( str_out ) g_free(str_out);
		if ( argv ) g_strfreev(argv);
		if ( contents ) g_free(contents);
		if ( certificate_data ) g_free(certificate_data);
		if ( team_id ) g_free(team_id);
		if ( bundle_id ) g_free(bundle_id);
		if ( cert_hash ) g_free(cert_hash);
		if ( cert_temp ) g_free(cert_temp);

		if ( entitlements_file ) g_free(entitlements_file);
		if ( temp_filename1 ) g_free(temp_filename1);
		if ( temp_filename2 ) g_free(temp_filename2);
		if ( version_string ) g_free(version_string);
		if ( image_filename ) g_free(image_filename);
		if ( user_name ) g_free(user_name);
		if ( group_name ) g_free(group_name);
		if ( icon_scaled_image ) gdk_pixbuf_unref(icon_scaled_image);
		if ( icon_image ) gdk_pixbuf_unref(icon_image);
		
		if ( app_name ) g_free(app_name);
		if ( profile ) g_free(profile);
		if ( app_icon ) g_free(app_icon);
		if ( facebook_id ) g_free(facebook_id);
		if ( version_number ) g_free(version_number);
		if ( output_file ) g_free(output_file);
	}

	running = 0;
}

void project_export_ipa()
{
	static GeanyProject *last_proj = 0;

	if (ui_widgets.ios_dialog == NULL)
	{
		ui_widgets.ios_dialog = create_ios_dialog();
		gtk_widget_set_name(ui_widgets.ios_dialog, "Export IPA");
		gtk_window_set_transient_for(GTK_WINDOW(ui_widgets.ios_dialog), GTK_WINDOW(main_widgets.window));

		g_signal_connect(ui_widgets.ios_dialog, "response", G_CALLBACK(on_ios_dialog_response), NULL);
        g_signal_connect(ui_widgets.ios_dialog, "delete-event", G_CALLBACK(gtk_widget_hide_on_delete), NULL);

		ui_setup_open_button_callback_ios(ui_lookup_widget(ui_widgets.ios_dialog, "ios_app_icon_path"), NULL,
			GTK_FILE_CHOOSER_ACTION_OPEN, GTK_ENTRY(ui_lookup_widget(ui_widgets.ios_dialog, "ios_app_icon_entry")));
		ui_setup_open_button_callback_ios(ui_lookup_widget(ui_widgets.ios_dialog, "ios_provisioning_path"), NULL,
			GTK_FILE_CHOOSER_ACTION_OPEN, GTK_ENTRY(ui_lookup_widget(ui_widgets.ios_dialog, "ios_provisioning_entry")));
		
		ui_setup_open_button_callback_ios(ui_lookup_widget(ui_widgets.ios_dialog, "ios_output_file_path"), NULL,
			GTK_FILE_CHOOSER_ACTION_SAVE, GTK_ENTRY(ui_lookup_widget(ui_widgets.ios_dialog, "ios_output_file_entry")));

		gtk_combo_box_set_active( GTK_COMBO_BOX(ui_lookup_widget(ui_widgets.ios_dialog, "ios_orientation_combo")), 0 );
	}

	if ( app->project != last_proj || app->project == 0 )
	{
		last_proj = app->project;
        if ( app->project )
        {
            gchar *filename = g_strconcat( app->project->name, ".ipa", NULL );
            gchar* apk_path = g_build_filename( app->project->base_path, filename, NULL );
            gtk_entry_set_text( GTK_ENTRY(ui_lookup_widget(ui_widgets.ios_dialog, "ios_output_file_entry")), apk_path );
            g_free(apk_path);
            g_free(filename);
        }
        else
        {
            gchar* apk_path = g_build_filename( local_prefs.project_file_path, "AGK Player.ipa", NULL );
            gtk_entry_set_text( GTK_ENTRY(ui_lookup_widget(ui_widgets.ios_dialog, "ios_output_file_entry")), apk_path );
            g_free(apk_path);
        }
	}

	gtk_window_present(GTK_WINDOW(ui_widgets.ios_dialog));
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

			err_code = utils_mkdir(locale_path, TRUE);

			if (err_code != 0)
			{
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
		//local_prefs.project_file_path = g_build_filename(g_get_home_dir(), "AGK Projects", NULL);
		local_prefs.project_file_path = g_build_filename(g_get_user_special_dir(G_USER_DIRECTORY_DOCUMENTS), "AGK Projects", NULL);
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