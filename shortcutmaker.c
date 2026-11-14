#include <gtk/gtk.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <limits.h>

#define APP_VERSION "1.0.0"
#define BUF_SM 512
#define BUF_MD 1024
#define BUF_LG 2048
#define BUF_XL 8192

typedef struct {
    GtkWidget *entry_name;
    GtkWidget *entry_cmd;
    GtkWidget *entry_wmclass;
    GtkWidget *img_preview;
    GtkWidget *lbl_icon_status;
    GtkWidget *list_box;
    char target_dir[BUF_MD];
    char staged_icon_path[BUF_LG];
} AppWidgets;

static void refresh_shortcut_list(AppWidgets *app);

static void sync_hicolor_icon_theme(void) {
    const char *home = getenv("HOME");
    if (!home) return;

    char hicolor_root[BUF_LG];
    snprintf(hicolor_root, sizeof(hicolor_root), "%s/.local/share/icons/hicolor", home);
    g_mkdir_with_parents(hicolor_root, 0755);

    char user_index[BUF_XL];
    snprintf(user_index, sizeof(user_index), "%s/index.theme", hicolor_root);
    if (access(user_index, R_OK) != 0 && access("/usr/share/icons/hicolor/index.theme", R_OK) == 0) {
        FILE *in = fopen("/usr/share/icons/hicolor/index.theme", "rb");
        FILE *out = fopen(user_index, "wb");
        if (in && out) {
            char buf[BUF_XL];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
                fwrite(buf, 1, n, out);
            }
        }
        if (in) fclose(in);
        if (out) fclose(out);
    }

    char cache_cmd[BUF_XL];
    snprintf(cache_cmd, sizeof(cache_cmd), "gtk-update-icon-cache -f -t \"%s\" >/dev/null 2>&1", hicolor_root);
    int res = system(cache_cmd);
    (void)res;
}

static void update_icon_preview(AppWidgets *app, const char *icon_path, const char *status_text) {
    gtk_label_set_text(GTK_LABEL(app->lbl_icon_status), status_text);

    if (icon_path && access(icon_path, R_OK) == 0) {
        GError *err = NULL;
        GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file_at_scale(icon_path, 48, 48, TRUE, &err);
        if (pixbuf) {
            gtk_image_set_from_pixbuf(GTK_IMAGE(app->img_preview), pixbuf);
            g_object_unref(pixbuf);
            snprintf(app->staged_icon_path, sizeof(app->staged_icon_path), "%s", icon_path);
            return;
        }
        if (err) g_error_free(err);
    }

    gtk_image_set_from_icon_name(GTK_IMAGE(app->img_preview), "applications-other", GTK_ICON_SIZE_DIALOG);
    *app->staged_icon_path = '\0';
}

static gboolean is_appimage_file(const char *filepath) {
    if (!filepath || access(filepath, R_OK) != 0) return FALSE;

    char *lower = g_ascii_strdown(filepath, -1);
    gboolean has_ext = g_str_has_suffix(lower, ".appimage");
    g_free(lower);
    if (has_ext) return TRUE;

    FILE *f = fopen(filepath, "rb");
    if (f) {
        unsigned char header[BUF_SM];
        memset(header, 0, sizeof(header));
        size_t n = fread(header, 1, sizeof(header), f);
        fclose(f);
        if (n >= 11 && memcmp(header + 8, "AI\x02", 3) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

static void auto_detect_wm_class(AppWidgets *app, const char *exec_path) {
    if (!exec_path || access(exec_path, X_OK) != 0) return;

    char *quoted_exec = g_shell_quote(exec_path);
    char cmd[BUF_XL];
    snprintf(cmd, sizeof(cmd),
             "BEFORE=$(wmctrl -l 2>/dev/null | awk '{print $1}'); "
             "%s >/dev/null 2>&1 & "
             "APP_PID=$!; "
             "FOUND_CLASS=\"\"; "
             "for i in $(seq 1 25); do "
             "  sleep 0.2; "
             "  AFTER=$(wmctrl -l 2>/dev/null | awk '{print $1}'); "
             "  for wid in $AFTER; do "
             "    if ! echo \"$BEFORE\" | grep -q \"$wid\"; then "
             "      CLASS=$(xprop -id \"$wid\" WM_CLASS 2>/dev/null | awk -F'\"' '{print $4}'); "
             "      if [ -n \"$CLASS\" ]; then FOUND_CLASS=\"$CLASS\"; break 2; fi; "
             "    fi; "
             "  done; "
             "done; "
             "kill -9 $APP_PID >/dev/null 2>&1; "
             "pkill -9 -P $APP_PID >/dev/null 2>&1; "
             "echo \"$FOUND_CLASS\"",
             quoted_exec);
    g_free(quoted_exec);

    FILE *fp = popen(cmd, "r");
    if (fp) {
        char result[BUF_SM];
        memset(result, 0, sizeof(result));
        if (fgets(result, sizeof(result), fp) != NULL) {
            result[strcspn(result, "\r\n")] = '\0';
            if (strlen(result) > 0) {
                gtk_entry_set_text(GTK_ENTRY(app->entry_wmclass), result);
            }
        }
        pclose(fp);
    }
}

static void on_detect_wm_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    AppWidgets *app = (AppWidgets *)user_data;
    const gchar *cmd = gtk_entry_get_text(GTK_ENTRY(app->entry_cmd));
    if (strlen(cmd) > 0) {
        auto_detect_wm_class(app, cmd);
    }
}

static void read_desktop_metadata(const char *squashfs_dir,
                                  char *out_icon_name, size_t icon_size,
                                  char *out_wm_class, size_t wm_size) {
    *out_icon_name = '\0';
    *out_wm_class = '\0';

    DIR *d = opendir(squashfs_dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (g_str_has_suffix(ent->d_name, ".desktop")) {
            char desktop_path[BUF_XL];
            snprintf(desktop_path, sizeof(desktop_path), "%s/%s", squashfs_dir, ent->d_name);

            char base_desk[BUF_SM];
            snprintf(base_desk, sizeof(base_desk), "%s", ent->d_name);
            char *dot = strrchr(base_desk, '.');
            if (dot) *dot = '\0';
            if (strchr(base_desk, '.') != NULL) {
                snprintf(out_wm_class, wm_size, "%s", base_desk);
            }

            FILE *f = fopen(desktop_path, "r");
            if (f) {
                char line[BUF_SM];
                while (fgets(line, sizeof(line), f)) {
                    if (strncmp(line, "Icon=", 5) == 0) {
                        char *val = line + 5;
                        val[strcspn(val, "\r\n")] = '\0';
                        snprintf(out_icon_name, icon_size, "%s", val);
                    } else if (strncmp(line, "StartupWMClass=", 15) == 0 && strlen(out_wm_class) == 0) {
                        char *val = line + 15;
                        val[strcspn(val, "\r\n")] = '\0';
                        snprintf(out_wm_class, wm_size, "%s", val);
                    }
                }
                fclose(f);
            }
            if (strlen(out_icon_name) > 0) break;
        }
    }
    closedir(d);
}

static void find_icon_recursive(const char *base_dir, const char *target_name,
                                char *best_path, size_t best_size, long *best_score) {
    DIR *d = opendir(base_dir);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;

        char full_path[BUF_XL];
        snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, ent->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) continue;

        if (S_ISDIR(st.st_mode)) {
            find_icon_recursive(full_path, target_name, best_path, best_size, best_score);
        } else if (S_ISREG(st.st_mode)) {
            char *lower = g_ascii_strdown(ent->d_name, -1);
            gboolean is_png = g_str_has_suffix(lower, ".png");
            gboolean is_svg = g_str_has_suffix(lower, ".svg");

            if (is_png || is_svg) {
                long score = st.st_size;

                if (target_name && strlen(target_name) > 0) {
                    char *target_lower = g_ascii_strdown(target_name, -1);
                    char exp_png[BUF_SM];
                    char exp_svg[BUF_SM];
                    snprintf(exp_png, sizeof(exp_png), "%s.png", target_lower);
                    snprintf(exp_svg, sizeof(exp_svg), "%s.svg", target_lower);

                    if (strcmp(lower, exp_png) == 0 || strcmp(lower, exp_svg) == 0) {
                        score += 10000000L;
                    }
                    g_free(target_lower);
                }

                if (strchr(full_path + strlen(base_dir) + 1, '/') == NULL) {
                    score += 500000L;
                }

                if (score > *best_score) {
                    *best_score = score;
                    snprintf(best_path, best_size, "%s", full_path);
                }
            }
            g_free(lower);
        }
    }
    closedir(d);
}

static void detect_and_preview_appimage_metadata(AppWidgets *app, const char *appimage_path) {
    if (!is_appimage_file(appimage_path)) {
        update_icon_preview(app, NULL, "Executable biasa (ikon default)");
        char *base = g_path_get_basename(appimage_path);
        gtk_entry_set_text(GTK_ENTRY(app->entry_wmclass), base);
        g_free(base);
        return;
    }

    chmod(appimage_path, 0755);

    char tmp_dir[] = "/tmp/appimage_icon_XXXXXX";
    if (!mkdtemp(tmp_dir)) {
        update_icon_preview(app, NULL, "Gagal membuat folder sementara");
        return;
    }

    char *quoted_app = g_shell_quote(appimage_path);
    char *quoted_tmp = g_shell_quote(tmp_dir);
    char cmd[BUF_XL];
    snprintf(cmd, sizeof(cmd),
             "cd %s && "
             "%s --appimage-extract '.DirIcon' >/dev/null 2>&1; "
             "%s --appimage-extract '*.desktop' >/dev/null 2>&1; "
             "%s --appimage-extract '*.png' >/dev/null 2>&1; "
             "%s --appimage-extract '*.svg' >/dev/null 2>&1",
             quoted_tmp, quoted_app, quoted_app, quoted_app, quoted_app);

    int res = system(cmd);
    (void)res;
    g_free(quoted_app);
    g_free(quoted_tmp);

    char squashfs_dir[BUF_LG];
    snprintf(squashfs_dir, sizeof(squashfs_dir), "%s/squashfs-root", tmp_dir);

    char found_icon[BUF_XL] = {0};
    char desktop_icon_name[BUF_SM] = {0};
    char detected_wm_class[BUF_SM] = {0};
read_desktop_metadata(squashfs_dir, desktop_icon_name, sizeof(desktop_icon_name),
detected_wm_class, sizeof(detected_wm_class));
if (strlen(detected_wm_class) > 0) {
gtk_entry_set_text(GTK_ENTRY(app->entry_wmclass), detected_wm_class);
}
long best_score = 0;
find_icon_recursive(squashfs_dir, desktop_icon_name, found_icon, sizeof(found_icon), &best_score);
if (strlen(found_icon) == 0) {
char diricon_path[BUF_XL];
snprintf(diricon_path, sizeof(diricon_path), "%s/.DirIcon", squashfs_dir);
char resolved_path[PATH_MAX];
if (realpath(diricon_path, resolved_path) != NULL && access(resolved_path, R_OK) == 0) {
snprintf(found_icon, sizeof(found_icon), "%s", resolved_path);
} else if (access(diricon_path, R_OK) == 0) {
snprintf(found_icon, sizeof(found_icon), "%s", diricon_path);
}
}
if (strlen(found_icon) > 0) {
char *lower = g_ascii_strdown(found_icon, -1);
const char *ext = g_str_has_suffix(lower, ".svg") ? "svg" : "png";
g_free(lower);
char staged_file[BUF_XL];
snprintf(staged_file, sizeof(staged_file), "/tmp/staged_shortcut_icon_%d.%s", getpid(), ext);
FILE *in = fopen(found_icon, "rb");
FILE *out = fopen(staged_file, "wb");
if (in && out) {
char buf[BUF_XL];
size_t n;
while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
fwrite(buf, 1, n, out);
}
fclose(in);
fclose(out);
update_icon_preview(app, staged_file, "Ikon asli AppImage terdeteksi!");
} else {
if (in) fclose(in);
if (out) fclose(out);
update_icon_preview(app, NULL, "Gagal menyalin ikon");
}
} else {
update_icon_preview(app, NULL, "AppImage tanpa ikon bawaan");
}
char cleanup_cmd[BUF_XL];
snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", tmp_dir);
res = system(cleanup_cmd);
(void)res;
}
static void on_browse_clicked(GtkButton *button, gpointer user_data) {
AppWidgets *app = (AppWidgets *)user_data;
GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));
GtkWidget *dialog = gtk_file_chooser_dialog_new("Pilih AppImage atau Executable",
window, GTK_FILE_CHOOSER_ACTION_OPEN,
"_Cancel", GTK_RESPONSE_CANCEL,
"_Open", GTK_RESPONSE_OK, NULL);
const char *home = getenv("HOME");
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
*base_name = toupper((unsigned char)*base_name);
gtk_entry_set_text(GTK_ENTRY(app->entry_name), base_name);
}
g_free(base_name);
}
detect_and_preview_appimage_metadata(app, filename);
auto_detect_wm_class(app, filename);
g_free(filename);
}
}
gtk_widget_destroy(dialog);
}
static void on_pick_icon_clicked(GtkButton *button, gpointer user_data) {
AppWidgets *app = (AppWidgets *)user_data;
GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));
GtkWidget *dialog = gtk_file_chooser_dialog_new("Pilih Ikon Kustom",
window, GTK_FILE_CHOOSER_ACTION_OPEN,
"_Cancel", GTK_RESPONSE_CANCEL,
"_Select", GTK_RESPONSE_OK, NULL);
GtkFileFilter *filter_img = gtk_file_filter_new();
gtk_file_filter_set_name(filter_img, "Images (PNG, SVG, XPM)");
gtk_file_filter_add_pattern(filter_img, ".png");
gtk_file_filter_add_pattern(filter_img, ".svg");
gtk_file_filter_add_pattern(filter_img, ".xpm");
gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter_img);
if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
char *icon_file = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
if (icon_file) {
update_icon_preview(app, icon_file, "Ikon kustom dipilih");
g_free(icon_file);
}
}
gtk_widget_destroy(dialog);
}
static void on_create_clicked(GtkButton *button, gpointer user_data) {
AppWidgets *app = (AppWidgets *)user_data;
GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));
const gchar *name = gtk_entry_get_text(GTK_ENTRY(app->entry_name));
const gchar *cmd = gtk_entry_get_text(GTK_ENTRY(app->entry_cmd));
const gchar *wmclass = gtk_entry_get_text(GTK_ENTRY(app->entry_wmclass));
if (strlen(name) == 0 || strlen(cmd) == 0) {
GtkWidget *dialog = gtk_message_dialog_new(window, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Name dan Command wajib diisi!");
gtk_dialog_run(GTK_DIALOG(dialog));
gtk_widget_destroy(dialog);
return;
}
char slug[BUF_SM];
if (strlen(wmclass) > 0) {
snprintf(slug, sizeof(slug), "%s", wmclass);
for (int i = 0; slug[i]; i++) {
slug[i] = tolower((unsigned char)slug[i]);
}
} else {
snprintf(slug, sizeof(slug), "%s", name);
for (int i = 0; slug[i]; i++) {
slug[i] = tolower((unsigned char)slug[i]);
if (slug[i] == ' ' || slug[i] == '/') {
slug[i] = '-';
}
}
}
char final_icon_value[BUF_SM];
snprintf(final_icon_value, sizeof(final_icon_value), "applications-other");
if (strlen(app->staged_icon_path) > 0 && access(app->staged_icon_path, R_OK) == 0) {
const char *home = getenv("HOME");
char *lower = g_ascii_strdown(app->staged_icon_path, -1);
gboolean is_svg = g_str_has_suffix(lower, ".svg");
g_free(lower);
if (is_svg) {
char dir_svg[BUF_LG];
char path_svg[BUF_XL];
snprintf(dir_svg, sizeof(dir_svg), "%s/.local/share/icons/hicolor/scalable/apps", home ? home : ".");
g_mkdir_with_parents(dir_svg, 0755);
snprintf(path_svg, sizeof(path_svg), "%s/%s.svg", dir_svg, slug);
FILE *in = fopen(app->staged_icon_path, "rb");
FILE *out = fopen(path_svg, "wb");
if (in && out) {
char buf[BUF_XL];
size_t n;
while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
fwrite(buf, 1, n, out);
}
snprintf(final_icon_value, sizeof(final_icon_value), "%s", slug);
}
if (in) fclose(in);
if (out) fclose(out);
} else {
int sizes[] = {48, 128, 256};
for (int idx = 0; idx < 3; idx++) {
int sz = sizes[idx];
char dir_png[BUF_LG];
char path_png[BUF_XL];
snprintf(dir_png, sizeof(dir_png), "%s/.local/share/icons/hicolor/%dx%d/apps", home ? home : ".", sz, sz);
g_mkdir_with_parents(dir_png, 0755);
snprintf(path_png, sizeof(path_png), "%s/%s.png", dir_png, slug);
GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(app->staged_icon_path, sz, sz, TRUE, NULL);
if (pb) {
if (gdk_pixbuf_save(pb, path_png, "png", NULL, NULL)) {
snprintf(final_icon_value, sizeof(final_icon_value), "%s", slug);
}
g_object_unref(pb);
}
}
}
sync_hicolor_icon_theme();
}
char filepath[BUF_XL];
snprintf(filepath, sizeof(filepath), "%s/%s.desktop", app->target_dir, slug);
FILE *f = fopen(filepath, "w");
if (!f) {
GtkWidget *dialog = gtk_message_dialog_new(window, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK, "Gagal menyimpan file .desktop!");
gtk_dialog_run(GTK_DIALOG(dialog));
gtk_widget_destroy(dialog);
return;
}
fprintf(f, "[Desktop Entry]\n");
fprintf(f, "Type=Application\n");
fprintf(f, "Name=%s\n", name);
    fprintf(f, "Exec=\"%s\"\n", cmd);
fprintf(f, "Icon=%s\n", final_icon_value);
if (strlen(wmclass) > 0) {
fprintf(f, "StartupWMClass=%s\n", wmclass);
}
fprintf(f, "Terminal=false\n");
fprintf(f, "Categories=Utility;\n");
fclose(f);
chmod(filepath, 0755);
char update_cmd[BUF_XL];
    snprintf(update_cmd, sizeof(update_cmd), "update-desktop-database \"%s\" >/dev/null 2>&1", app->target_dir);
int res = system(update_cmd);
(void)res;
gtk_entry_set_text(GTK_ENTRY(app->entry_name), "");
gtk_entry_set_text(GTK_ENTRY(app->entry_cmd), "");
gtk_entry_set_text(GTK_ENTRY(app->entry_wmclass), "");
update_icon_preview(app, NULL, "Belum ada file dipilih");
refresh_shortcut_list(app);
GtkWidget *dialog = gtk_message_dialog_new(window, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Shortcut berhasil dibuat!");
gtk_dialog_run(GTK_DIALOG(dialog));
gtk_widget_destroy(dialog);
}
static void on_delete_clicked(GtkButton *button, gpointer user_data) {
char *filepath = (char *)g_object_get_data(G_OBJECT(button), "filepath");
AppWidgets *app = (AppWidgets *)user_data;
GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));
if (filepath && remove(filepath) == 0) {
char *base = g_path_get_basename(filepath);
char *dot = strrchr(base, '.');
if (dot) *dot = '\0';
const char *home = getenv("HOME");
if (home) {
int sizes[] = {48, 128, 256};
for (int i = 0; i < 3; i++) {
char p[BUF_LG];
snprintf(p, sizeof(p), "%s/.local/share/icons/hicolor/%dx%d/apps/%s.png", home, sizes[i], sizes[i], base);
remove(p);
}
char s[BUF_LG];
snprintf(s, sizeof(s), "%s/.local/share/icons/hicolor/scalable/apps/%s.svg", home, base);
remove(s);
sync_hicolor_icon_theme();
}
g_free(base);
char update_cmd[BUF_XL];
    snprintf(update_cmd, sizeof(update_cmd), "update-desktop-database \"%s\" >/dev/null 2>&1", app->target_dir);
int res = system(update_cmd);
(void)res;
refresh_shortcut_list(app);
GtkWidget *dialog = gtk_message_dialog_new(window, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "Shortcut dihapus!");
gtk_dialog_run(GTK_DIALOG(dialog));
gtk_widget_destroy(dialog);
}
}
static void refresh_shortcut_list(AppWidgets *app) {
GList *children = gtk_container_get_children(GTK_CONTAINER(app->list_box));
for (GList *iter = children; iter != NULL; iter = g_list_next(iter)) {
gtk_widget_destroy(GTK_WIDGET(iter->data));
}
g_list_free(children);
DIR *dir = opendir(app->target_dir);
if (!dir) return;
struct dirent *entry;
while ((entry = readdir(dir)) != NULL) {
if (strcmp(entry->d_name, "shortcutmaker.desktop") == 0) continue;
if (g_str_has_suffix(entry->d_name, ".desktop")) {
char filepath[BUF_XL];
snprintf(filepath, sizeof(filepath), "%s/%s", app->target_dir, entry->d_name);
char icon_name[BUF_MD];
snprintf(icon_name, sizeof(icon_name), "applications-other");
FILE *df = fopen(filepath, "r");
if (df) {
char line[BUF_SM];
while (fgets(line, sizeof(line), df)) {
if (strncmp(line, "Icon=", 5) == 0) {
char *val = line + 5;
val[strcspn(val, "\r\n")] = '\0';
snprintf(icon_name, sizeof(icon_name), "%s", val);
break;
}
}
fclose(df);
}
GtkWidget *row_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
gtk_widget_set_margin_top(row_box, 6);
gtk_widget_set_margin_bottom(row_box, 6);
gtk_widget_set_margin_start(row_box, 6);
gtk_widget_set_margin_end(row_box, 6);
GtkWidget *row_icon = gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
gtk_box_pack_start(GTK_BOX(row_box), row_icon, FALSE, FALSE, 0);
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
memset(&app, 0, sizeof(AppWidgets));
const char *home = getenv("HOME");
snprintf(app.target_dir, sizeof(app.target_dir), "%s/.local/share/applications", home ? home : ".");
g_mkdir_with_parents(app.target_dir, 0755);
GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
gtk_window_set_title(GTK_WINDOW(window), "CreateShortcut v" APP_VERSION);
gtk_container_set_border_width(GTK_CONTAINER(window), 12);
gtk_window_set_default_size(GTK_WINDOW(window), 520, 410);
gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);
GtkWidget *notebook = gtk_notebook_new();
gtk_container_add(GTK_CONTAINER(window), notebook);
GtkWidget *vbox_create = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
gtk_container_set_border_width(GTK_CONTAINER(vbox_create), 10);
GtkWidget *grid = gtk_grid_new();
gtk_grid_set_column_spacing(GTK_GRID(grid), 10);
gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
gtk_box_pack_start(GTK_BOX(vbox_create), grid, TRUE, TRUE, 0);
GtkWidget *label_name = gtk_label_new("Name:");
gtk_widget_set_halign(label_name, GTK_ALIGN_END);
gtk_grid_attach(GTK_GRID(grid), label_name, 0, 0, 1, 1);
app.entry_name = gtk_entry_new();
gtk_widget_set_hexpand(app.entry_name, TRUE);
gtk_entry_set_placeholder_text(GTK_ENTRY(app.entry_name), "Contoh: LocalSend");
gtk_grid_attach(GTK_GRID(grid), app.entry_name, 1, 0, 1, 1);
GtkWidget *label_cmd = gtk_label_new("Command:");
gtk_widget_set_halign(label_cmd, GTK_ALIGN_END);
gtk_grid_attach(GTK_GRID(grid), label_cmd, 0, 1, 1, 1);
GtkWidget *cmd_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
gtk_widget_set_hexpand(cmd_box, TRUE);
app.entry_cmd = gtk_entry_new();
gtk_entry_set_placeholder_text(GTK_ENTRY(app.entry_cmd), "Pilih AppImage / executable");
gtk_box_pack_start(GTK_BOX(cmd_box), app.entry_cmd, TRUE, TRUE, 0);
GtkWidget *btn_browse = gtk_button_new_with_label("Open");
g_signal_connect(btn_browse, "clicked", G_CALLBACK(on_browse_clicked), &app);
gtk_box_pack_start(GTK_BOX(cmd_box), btn_browse, FALSE, FALSE, 0);
gtk_grid_attach(GTK_GRID(grid), cmd_box, 1, 1, 1, 1);
GtkWidget *label_wm = gtk_label_new("WM Class:");
gtk_widget_set_halign(label_wm, GTK_ALIGN_END);
gtk_grid_attach(GTK_GRID(grid), label_wm, 0, 2, 1, 1);
GtkWidget *wm_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 5);
gtk_widget_set_hexpand(wm_box, TRUE);
app.entry_wmclass = gtk_entry_new();
gtk_entry_set_placeholder_text(GTK_ENTRY(app.entry_wmclass), "Otomatis terdeteksi dari jendela aplikasi");
gtk_box_pack_start(GTK_BOX(wm_box), app.entry_wmclass, TRUE, TRUE, 0);
GtkWidget *btn_detect_wm = gtk_button_new_with_label("Detect");
g_signal_connect(btn_detect_wm, "clicked", G_CALLBACK(on_detect_wm_clicked), &app);
gtk_box_pack_start(GTK_BOX(wm_box), btn_detect_wm, FALSE, FALSE, 0);
gtk_grid_attach(GTK_GRID(grid), wm_box, 1, 2, 1, 1);
GtkWidget *label_icon = gtk_label_new("Icon Preview:");
gtk_widget_set_halign(label_icon, GTK_ALIGN_END);
gtk_grid_attach(GTK_GRID(grid), label_icon, 0, 3, 1, 1);
GtkWidget *preview_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
app.img_preview = gtk_image_new_from_icon_name("applications-other", GTK_ICON_SIZE_DIALOG);
gtk_widget_set_size_request(app.img_preview, 48, 48);
gtk_box_pack_start(GTK_BOX(preview_box), app.img_preview, FALSE, FALSE, 0);
app.lbl_icon_status = gtk_label_new("Belum ada file dipilih");
gtk_widget_set_halign(app.lbl_icon_status, GTK_ALIGN_START);
gtk_box_pack_start(GTK_BOX(preview_box), app.lbl_icon_status, TRUE, TRUE, 0);
GtkWidget *btn_pick_icon = gtk_button_new_with_label("Change Icon");
g_signal_connect(btn_pick_icon, "clicked", G_CALLBACK(on_pick_icon_clicked), &app);
gtk_box_pack_end(GTK_BOX(preview_box), btn_pick_icon, FALSE, FALSE, 0);
gtk_grid_attach(GTK_GRID(grid), preview_box, 1, 3, 1, 1);
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
