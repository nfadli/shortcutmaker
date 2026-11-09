#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
typedef struct {
    GtkWidget *entry_name;
    GtkWidget *entry_cmd;
    GtkWidget *list_box;
    char target_dir[512];
} AppWidgets;
static void refresh_shortcut_list(AppWidgets *app);
static void on_browse_clicked(GtkButton *button, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    GtkWidget *dialog;
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));
    dialog = gtk_file_chooser_dialog_new("Select Executable File",
                                         window,
                                         GTK_FILE_CHOOSER_ACTION_OPEN,
                                         "_Cancel", GTK_RESPONSE_CANCEL,
                                         "_Open", GTK_RESPONSE_OK,
                                         NULL);
    char *home = getenv("HOME");
    if (home) {
        gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), home);
    }
    if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
        char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
        if (filename) {
            gtk_entry_set_text(GTK_ENTRY(app->entry_cmd), filename);
            const char *current_name = gtk_entry_get_text(GTK_ENTRY(app->entry_name));
            if (strlen(current_name) == 0) {
                char *base_name = g_path_get_basename(filename);
                char *dot = strrchr(base_name, '.');
                if (dot) *dot = '\0';
                if (strlen(base_name) > 0) {
                    base_name[0] = toupper(base_name[0]);
                    gtk_entry_set_text(GTK_ENTRY(app->entry_name), base_name);
                }
                g_free(base_name);
            }
            g_free(filename);
        }
    }
    gtk_widget_destroy(dialog);
}
static void on_create_clicked(GtkButton *button, gpointer user_data) {
    AppWidgets *app = (AppWidgets *)user_data;
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));
    const gchar *name = gtk_entry_get_text(GTK_ENTRY(app->entry_name));
    const gchar *cmd = gtk_entry_get_text(GTK_ENTRY(app->entry_cmd));
    if (strlen(name) == 0 || strlen(cmd) == 0) {
        GtkWidget *dialog = gtk_message_dialog_new(window, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Name and Command must be filled!");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }
    char filename[256];
    snprintf(filename, sizeof(filename), "%s", name);
    for (int i = 0; filename[i]; i++) {
        filename[i] = tolower(filename[i]);
        if (filename[i] == ' ') filename[i] = '-';
    }
    char filepath[1024];
    snprintf(filepath, sizeof(filepath), "%s/%s.desktop", app->target_dir, filename);
    FILE *f = fopen(filepath, "w");
    if (!f) {
        GtkWidget *dialog = gtk_message_dialog_new(window, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Failed to save .desktop file!");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
        return;
    }
    fprintf(f, "[Desktop Entry]\n");
    fprintf(f, "Type=Application\n");
    fprintf(f, "Name=%s\n", name);
    fprintf(f, "Exec=%s\n", cmd);
    fprintf(f, "Icon=applications-other\n");
    fprintf(f, "Terminal=false\n");
    fprintf(f, "Categories=Utility;\n");
    fclose(f);
    chmod(filepath, 0755);
    // Reset entry and refresh manage list
    gtk_entry_set_text(GTK_ENTRY(app->entry_name), "");
    gtk_entry_set_text(GTK_ENTRY(app->entry_cmd), "");
    refresh_shortcut_list(app);
    GtkWidget *dialog = gtk_message_dialog_new(window, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Shortcut created successfully!");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
}
static void on_delete_clicked(GtkButton *button, gpointer user_data) {
    char *filepath = (char *)g_object_get_data(G_OBJECT(button), "filepath");
    AppWidgets *app = (AppWidgets *)user_data;
    GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));
    if (filepath && remove(filepath) == 0) {
        refresh_shortcut_list(app);
        GtkWidget *dialog = gtk_message_dialog_new(window, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Shortcut deleted successfully!");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    } else {
        GtkWidget *dialog = gtk_message_dialog_new(window, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Failed to delete shortcut file!");
        gtk_dialog_run(GTK_DIALOG(dialog));
        gtk_widget_destroy(dialog);
    }
}
static void refresh_shortcut_list(AppWidgets *app) {
    GList *children = gtk_container_get_children(GTK_CONTAINER(app->list_box));
    for (GList *iter = children; iter != NULL; iter = g_list_next(iter)) {
        gtk_widget_destroy(GTK_WIDGET(iter->data)); // Fixed macro safety below
    }
    for (GList *iter = children; iter != NULL; iter = g_list_next(iter)) {
        gtk_widget_destroy(GTK_WIDGET(iter->data));
    }
    g_list_free(children);
    DIR *dir = opendir(app->target_dir);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, "shortcutmaker.desktop") == 0) {
            continue;
        }
        if (strstr(entry->d_name, ".desktop") != NULL) {
            char filepath[1024];
            snprintf(filepath, sizeof(filepath), "%s/%s", app->target_dir, entry->d_name);
            GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
            gtk_widget_set_margin_top(row_box, 5);
            gtk_widget_set_margin_bottom(row_box, 5);
            gtk_widget_set_margin_start(row_box, 5);
            gtk_widget_set_margin_end(row_box, 5);
            GtkWidget *lbl = gtk_label_new(entry->d_name);
            gtk_widget_set_halign(lbl, GTK_ALIGN_START);
            gtk_box_pack_start(GTK_BOX(row_box), lbl, TRUE, TRUE, 0);
            GtkWidget *btn_del = gtk_button_new_with_label("Delete");
            g_object_set_data_full(G_OBJECT(btn_del), "filepath", g_strdup(filepath), g_free);
            g_signal_connect(btn_del, "clicked", G_CALLBACK(on_delete_clicked), app);
            gtk_box_pack_end(GTK_BOX(row_box), btn_del, FALSE, FALSE, 0);
            gtk_container_add(GTK_CONTAINER(app->list_box), row_box);
        }
    }
    closedir(dir);
    gtk_widget_show_all(app->list_box);
}
int main(int argc, char *argv[]) {
    gtk_init(&argc, &argv);
    AppWidgets app;
    char *home = getenv("HOME");
    if (home) {
        snprintf(app.target_dir, sizeof(app.target_dir), "%s/.local/share/applications", home);
    } else {
        strcpy(app.target_dir, "./");
    }
    char mkdir_cmd[1024];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p \"%s\"", app.target_dir);
    system(mkdir_cmd);
    GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(window), "CreateShortcut");
    gtk_container_set_border_width(GTK_CONTAINER(window), 12);
    gtk_window_set_default_size(GTK_WINDOW(window), 480, 320);
    gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
    g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
    GtkWidget *notebook = gtk_notebook_new();
    gtk_container_add(GTK_CONTAINER(window), notebook);
    GtkWidget *vbox_create = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox_create), 10);
    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
    gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
    gtk_box_pack_start(GTK_BOX(vbox_create), grid, TRUE, TRUE, 0);
    GtkWidget *label_name = gtk_label_new("Name:");
    gtk_widget_set_halign(label_name, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), label_name, 0, 0, 1, 1);
    app.entry_name = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.entry_name), "e.g., My App");
    gtk_grid_attach(GTK_GRID(grid), app.entry_name, 1, 0, 1, 1);
    GtkWidget *label_cmd = gtk_label_new("Command:");
    gtk_widget_set_halign(label_cmd, GTK_ALIGN_END);
    gtk_grid_attach(GTK_GRID(grid), label_cmd, 0, 1, 1, 1);
    GtkWidget *cmd_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
    app.entry_cmd = gtk_entry_new();
    gtk_entry_set_placeholder_text(GTK_ENTRY(app.entry_cmd), "Select file or type manually");
    gtk_box_pack_start(GTK_BOX(cmd_box), app.entry_cmd, TRUE, TRUE, 0);
    GtkWidget *btn_browse = gtk_button_new_with_label("Open");
    g_signal_connect(btn_browse, "clicked", G_CALLBACK(on_browse_clicked), &app);
    gtk_box_pack_start(GTK_BOX(cmd_box), btn_browse, FALSE, FALSE, 0);
    gtk_grid_attach(GTK_GRID(grid), cmd_box, 1, 1, 1, 1);
    GtkWidget *btn_create = gtk_button_new_with_label("Create Shortcut");
    g_signal_connect(btn_create, "clicked", G_CALLBACK(on_create_clicked), &app);
    gtk_box_pack_start(GTK_BOX(vbox_create), btn_create, FALSE, FALSE, 0);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox_create, gtk_label_new("Create"));
    GtkWidget *vbox_manage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
    gtk_container_set_border_width(GTK_CONTAINER(vbox_manage), 10);
    GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window), GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
    gtk_box_pack_start(GTK_BOX(vbox_manage), scrolled_window, TRUE, TRUE, 0);
    app.list_box = gtk_list_box_new();
    gtk_container_add(GTK_CONTAINER(scrolled_window), app.list_box);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox_manage, gtk_label_new("Manage"));
    refresh_shortcut_list(&app);
    gtk_widget_show_all(window);
    gtk_main();
    return 0;
}
