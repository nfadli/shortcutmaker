#include <ctype.h>
#include <dirent.h>
#include <gtk/gtk.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define APP_VERSION "0.0.1"
#define BUF_SM 512
#define BUF_MD 1024
#define BUF_LG 4096
#define BUF_XL 16384

typedef struct {
  GtkWidget *entry_appimage;
  GtkWidget *img_preview;
  GtkWidget *lbl_icon_info;
  GtkWidget *text_preview_desktop;
  GtkTextBuffer *preview_buffer;
  GtkWidget *list_box;
  char target_dir[BUF_MD];
  char staged_icon_path[BUF_XL];
  char selected_appimage[BUF_XL];
  char generated_slug[BUF_SM];
  char generated_app_name[BUF_SM];
  char generated_wm_class[BUF_SM];
  char last_copied_icon_path[BUF_XL];
} AppWidgets;

static void refresh_shortcut_list(AppWidgets *app);

static void sync_hicolor_icon_theme(void) {
  const char *home = getenv("HOME");
  if (!home)
    return;

  char hicolor_root[BUF_LG];
  snprintf(hicolor_root, sizeof(hicolor_root), "%s/.local/share/icons/hicolor",
           home);
  g_mkdir_with_parents(hicolor_root, 0755);

  char user_index[BUF_XL];
  snprintf(user_index, sizeof(user_index), "%s/index.theme", hicolor_root);

  if (access(user_index, R_OK) != 0) {
    FILE *idx_f = fopen(user_index, "w");
    if (idx_f) {
      fprintf(idx_f, "[Icon Theme]\nName=Hicolor\nComment=Custom Local Hicolor "
                     "Theme\nDirectories=48x48/apps,128x128/apps,256x256/"
                     "apps,scalable/apps\n\n");
      fprintf(
          idx_f,
          "[48x48/apps]\nSize=48\nContext=Applications\nType=Threshold\n\n");
      fprintf(
          idx_f,
          "[128x128/apps]\nSize=128\nContext=Applications\nType=Threshold\n\n");
      fprintf(
          idx_f,
          "[256x256/apps]\nSize=256\nContext=Applications\nType=Threshold\n\n");
      fprintf(idx_f, "[scalable/"
                     "apps]\nSize=48\nMinSize=16\nMaxSize=512\nContext="
                     "Applications\nType=Scalable\n");
      fclose(idx_f);
    }
  }

  char cache_cmd[BUF_XL];
  snprintf(cache_cmd, sizeof(cache_cmd),
           "gtk-update-icon-cache -f -t \"%s\" >/dev/null 2>&1", hicolor_root);
  int res = system(cache_cmd);
  (void)res;
}

static void update_icon_preview(AppWidgets *app, const char *icon_path,
                                const char *info_text) {
  gtk_label_set_text(GTK_LABEL(app->lbl_icon_info), info_text);

  if (icon_path && access(icon_path, R_OK) == 0) {
    GError *err = NULL;
    GdkPixbuf *pixbuf =
        gdk_pixbuf_new_from_file_at_scale(icon_path, 48, 48, TRUE, &err);
    if (pixbuf) {
      gtk_image_set_from_pixbuf(GTK_IMAGE(app->img_preview), pixbuf);
      g_object_unref(pixbuf);
      snprintf(app->staged_icon_path, sizeof(app->staged_icon_path), "%s",
               icon_path);
      return;
    }
    if (err)
      g_error_free(err);
  }

  gtk_image_set_from_icon_name(GTK_IMAGE(app->img_preview),
                               "applications-other", GTK_ICON_SIZE_DIALOG);
  *app->staged_icon_path = '\0';
}

static gboolean is_appimage_file(const char *filepath) {
  if (!filepath || access(filepath, R_OK) != 0)
    return FALSE;

  char *lower = g_ascii_strdown(filepath, -1);
  gboolean has_ext = g_str_has_suffix(lower, ".appimage");
  g_free(lower);
  if (has_ext)
    return TRUE;

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

static void read_desktop_metadata(const char *squashfs_dir, char *out_icon_name,
                                  size_t icon_size, char *out_name,
                                  size_t name_size, char *out_wm_class,
                                  size_t wm_size) {
  *out_icon_name = '\0';
  *out_name = '\0';
  *out_wm_class = '\0';

  DIR *d = opendir(squashfs_dir);
  if (!d)
    return;

  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (g_str_has_suffix(ent->d_name, ".desktop")) {
      char desktop_path[BUF_XL];
      snprintf(desktop_path, sizeof(desktop_path), "%s/%s", squashfs_dir,
               ent->d_name);

      char base_desk[BUF_SM];
      snprintf(base_desk, sizeof(base_desk), "%s", ent->d_name);
      char *dot = strrchr(base_desk, '.');
      if (dot)
        *dot = '\0';
      if (strchr(base_desk, '.') != NULL) {
        snprintf(out_wm_class, wm_size, "%s", base_desk);
      }

      FILE *f = fopen(desktop_path, "r");
      if (f) {
        char line[BUF_SM];
        while (fgets(line, sizeof(line), f)) {
          if (strncmp(line, "Name=", 5) == 0 && strlen(out_name) == 0) {
            char *val = line + 5;
            val[strcspn(val, "\r\n")] = '\0';
            snprintf(out_name, name_size, "%s", val);
          } else if (strncmp(line, "Icon=", 5) == 0) {
            char *val = line + 5;
            val[strcspn(val, "\r\n")] = '\0';
            snprintf(out_icon_name, icon_size, "%s", val);
          } else if (strncmp(line, "StartupWMClass=", 15) == 0 &&
                     strlen(out_wm_class) == 0) {
            char *val = line + 15;
            val[strcspn(val, "\r\n")] = '\0';
            snprintf(out_wm_class, wm_size, "%s", val);
          }
        }
        fclose(f);
      }
      if (strlen(out_icon_name) > 0)
        break;
    }
  }
  closedir(d);
}

static void find_icon_recursive(const char *base_dir, const char *target_name,
                                char *best_path, size_t best_size,
                                long *best_score) {
  DIR *d = opendir(base_dir);
  if (!d)
    return;

  struct dirent *ent;
  while ((ent = readdir(d)) != NULL) {
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
      continue;

    char full_path[BUF_XL];
    snprintf(full_path, sizeof(full_path), "%s/%s", base_dir, ent->d_name);

    struct stat st;
    if (stat(full_path, &st) != 0)
      continue;

    if (S_ISDIR(st.st_mode)) {
      find_icon_recursive(full_path, target_name, best_path, best_size,
                          best_score);
    } else if (S_ISREG(st.st_mode) && st.st_size > 50) {
      char *lower = g_ascii_strdown(ent->d_name, -1);
      gboolean is_png = g_str_has_suffix(lower, ".png");
      gboolean is_svg = g_str_has_suffix(lower, ".svg");
      gboolean is_xpm = g_str_has_suffix(lower, ".xpm");

      if (is_png || is_svg || is_xpm) {
        int img_w = 0, img_h = 0;
        if (!gdk_pixbuf_get_file_info(full_path, &img_w, &img_h)) {
          g_free(lower);
          continue;
        }

        long score = (long)(img_w * img_h) + (st.st_size / 10);

        if (strstr(full_path, "share/icons") != NULL ||
            strstr(full_path, "share/pixmaps") != NULL ||
            strstr(full_path, "share/") != NULL) {
          score += 5000000L;
        }

        if (target_name && strlen(target_name) > 0) {
          char *target_lower = g_ascii_strdown(target_name, -1);
          if (g_str_has_prefix(lower, target_lower)) {
            score += 10000000L;
          }
          g_free(target_lower);
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

static void process_appimage(AppWidgets *app, const char *appimage_path) {
  if (!is_appimage_file(appimage_path)) {
    GtkWidget *dialog = gtk_message_dialog_new(
        NULL, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
        "File yang dipilih bukan AppImage yang valid!");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return;
  }

  snprintf(app->selected_appimage, sizeof(app->selected_appimage), "%s",
           appimage_path);
  gtk_entry_set_text(GTK_ENTRY(app->entry_appimage), appimage_path);
  chmod(appimage_path, 0755);

  char tmp_dir[] = "/tmp/appimage_icon_XXXXXX";
  if (!mkdtemp(tmp_dir)) {
    update_icon_preview(app, NULL, "Gagal membuat folder sementara");
    return;
  }

  char *quoted_app = g_shell_quote(appimage_path);
  char *quoted_tmp = g_shell_quote(tmp_dir);
  char cmd[BUF_XL];

  snprintf(cmd, sizeof(cmd), "cd %s && %s --appimage-extract >/dev/null 2>&1",
           quoted_tmp, quoted_app);
  int res = system(cmd);
  (void)res;

  char squashfs_dir[BUF_LG];
  snprintf(squashfs_dir, sizeof(squashfs_dir), "%s/squashfs-root", tmp_dir);

  if (access(squashfs_dir, F_OK) != 0) {
    snprintf(cmd, sizeof(cmd),
             "OFFSET=$(%s --appimage-offset 2>/dev/null); "
             "if [ -n \"$OFFSET\" ]; then "
             "  unsquashfs -o \"$OFFSET\" -d %s %s >/dev/null 2>&1; "
             "fi",
             quoted_app, squashfs_dir, quoted_app);
    res = system(cmd);
    (void)res;
  }

  g_free(quoted_app);
  g_free(quoted_tmp);

  char found_icon[BUF_XL] = {0};
  char desktop_icon_name[BUF_SM] = {0};
  char app_name[BUF_SM] = {0};
  char wm_class[BUF_SM] = {0};

  read_desktop_metadata(squashfs_dir, desktop_icon_name,
                        sizeof(desktop_icon_name), app_name, sizeof(app_name),
                        wm_class, sizeof(wm_class));

  if (strlen(app_name) == 0) {
    char *base = g_path_get_basename(appimage_path);
    char *dot = strrchr(base, '.');
    if (dot)
      *dot = '\0';
    snprintf(app_name, sizeof(app_name), "%s", base);
    g_free(base);
  }

  snprintf(app->generated_app_name, sizeof(app->generated_app_name), "%s",
           app_name);

  char slug[BUF_SM];
  snprintf(slug, sizeof(slug), "%s", app_name);
  for (int i = 0; slug[i]; i++) {
    slug[i] = (char)tolower((unsigned char)slug[i]);
    if (slug[i] == ' ' || slug[i] == '/' || slug[i] == '.' || slug[i] == '_') {
      slug[i] = '-';
    }
  }
  snprintf(app->generated_slug, sizeof(app->generated_slug), "%s", slug);

  if (strlen(wm_class) == 0) {
    snprintf(app->generated_wm_class, sizeof(app->generated_wm_class), "%s",
             slug);
  } else {
    snprintf(app->generated_wm_class, sizeof(app->generated_wm_class), "%s",
             wm_class);
  }

  long best_score = 0;
  find_icon_recursive(squashfs_dir, desktop_icon_name, found_icon,
                      sizeof(found_icon), &best_score);

  if (strlen(found_icon) == 0) {
    long fallback_score = -1;
    find_icon_recursive(squashfs_dir, "", found_icon, sizeof(found_icon),
                        &fallback_score);
  }

  if (strlen(found_icon) == 0) {
    char diricon_path[BUF_XL];
    snprintf(diricon_path, sizeof(diricon_path), "%s/.DirIcon", squashfs_dir);
    char resolved_path[PATH_MAX];
    if (realpath(diricon_path, resolved_path) != NULL &&
        gdk_pixbuf_get_file_info(resolved_path, NULL, NULL)) {
      snprintf(found_icon, sizeof(found_icon), "%s", resolved_path);
    } else if (gdk_pixbuf_get_file_info(diricon_path, NULL, NULL)) {
      snprintf(found_icon, sizeof(found_icon), "%s", diricon_path);
    }
  }

  if (strlen(found_icon) > 0) {
    char *lower = g_ascii_strdown(found_icon, -1);
    const char *ext = g_str_has_suffix(lower, ".svg") ? "svg" : "png";
    g_free(lower);

    char staged_file[BUF_XL];
    snprintf(staged_file, sizeof(staged_file),
             "/tmp/staged_shortcut_icon_%d.%s", getpid(), ext);

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
      update_icon_preview(app, staged_file,
                          "Native Icon berhasil ditemukan dari folder share!");
    } else {
      if (in)
        fclose(in);
      if (out)
        fclose(out);
      update_icon_preview(app, NULL, "Gagal menyalin ikon");
    }
  } else {
    update_icon_preview(app, NULL, "Native icon tidak ditemukan");
  }

  char preview_desktop[BUF_XL];
  snprintf(preview_desktop, sizeof(preview_desktop),
           "[Desktop Entry]\n"
           "Type=Application\n"
           "Name=%s\n"
           "Exec=\"%s\"\n"
           "Icon=%s\n"
           "StartupWMClass=%s\n"
           "Terminal=false\n"
           "Categories=Utility;\n",
           app->generated_app_name, app->selected_appimage, app->generated_slug,
           app->generated_wm_class);
  gtk_text_buffer_set_text(app->preview_buffer, preview_desktop, -1);

  char cleanup_cmd[BUF_XL];
  snprintf(cleanup_cmd, sizeof(cleanup_cmd), "rm -rf %s", tmp_dir);
  res = system(cleanup_cmd);
  (void)res;
}

static void on_browse_clicked(GtkButton *button, gpointer user_data) {
  AppWidgets *app = (AppWidgets *)user_data;
  GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));
  GtkWidget *dialog = gtk_file_chooser_dialog_new(
      "Pilih File AppImage", window, GTK_FILE_CHOOSER_ACTION_OPEN, "_Cancel",
      GTK_RESPONSE_CANCEL, "_Open", GTK_RESPONSE_OK, NULL);

  GtkFileFilter *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "AppImage Files");
  gtk_file_filter_add_pattern(filter, "*.AppImage");
  gtk_file_filter_add_pattern(filter, "*.appimage");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(dialog), filter);

  const char *home = getenv("HOME");
  if (home) {
    gtk_file_chooser_set_current_folder(GTK_FILE_CHOOSER(dialog), home);
  }
  if (gtk_dialog_run(GTK_DIALOG(dialog)) == GTK_RESPONSE_OK) {
    char *filename = gtk_file_chooser_get_filename(GTK_FILE_CHOOSER(dialog));
    if (filename) {
      process_appimage(app, filename);
      g_free(filename);
    }
  }
  gtk_widget_destroy(dialog);
}

static void on_create_shortcut_clicked(GtkButton *button, gpointer user_data) {
  AppWidgets *app = (AppWidgets *)user_data;
  GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));

  if (strlen(app->selected_appimage) == 0 ||
      access(app->selected_appimage, R_OK) != 0) {
    GtkWidget *dialog = gtk_message_dialog_new(
        window, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
        "Pilih file AppImage terlebih dahulu!");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return;
  }

  char final_icon_value[BUF_SM];
  snprintf(final_icon_value, sizeof(final_icon_value), "applications-other");

  const char *home = getenv("HOME");
  if (strlen(app->staged_icon_path) > 0 &&
      access(app->staged_icon_path, R_OK) == 0 && home) {
    char *lower = g_ascii_strdown(app->staged_icon_path, -1);
    gboolean is_svg = g_str_has_suffix(lower, ".svg");
    g_free(lower);

    if (is_svg) {
      char dir_svg[BUF_LG];
      snprintf(dir_svg, sizeof(dir_svg),
               "%s/.local/share/icons/hicolor/scalable/apps", home);
      g_mkdir_with_parents(dir_svg, 0755);
      snprintf(app->last_copied_icon_path, sizeof(app->last_copied_icon_path),
               "%s/%s.svg", dir_svg, app->generated_slug);

      FILE *in = fopen(app->staged_icon_path, "rb");
      FILE *out = fopen(app->last_copied_icon_path, "wb");
      if (in && out) {
        char buf[BUF_XL];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
          fwrite(buf, 1, n, out);
        }
        snprintf(final_icon_value, sizeof(final_icon_value), "%s",
                 app->generated_slug);
      }
      if (in)
        fclose(in);
      if (out)
        fclose(out);
    } else {
      int sizes[] = {48, 128, 256};
      gboolean icon_saved_successfully = FALSE;

      for (int idx = 0; idx < 3; idx++) {
        int sz = sizes[idx];
        char dir_png[BUF_LG];
        char path_png[BUF_XL];
        snprintf(dir_png, sizeof(dir_png),
                 "%s/.local/share/icons/hicolor/%dx%d/apps", home, sz, sz);
        g_mkdir_with_parents(dir_png, 0755);
        snprintf(path_png, sizeof(path_png), "%s/%s.png", dir_png,
                 app->generated_slug);

        GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_scale(app->staged_icon_path,
                                                          sz, sz, TRUE, NULL);
        if (pb) {
          if (gdk_pixbuf_save(pb, path_png, "png", NULL, NULL)) {
            icon_saved_successfully = TRUE;
            if (sz == 48) {
              snprintf(app->last_copied_icon_path,
                       sizeof(app->last_copied_icon_path), "%s", path_png);
            }
          }
          g_object_unref(pb);
        }
      }

      if (icon_saved_successfully) {
        snprintf(final_icon_value, sizeof(final_icon_value), "%s",
                 app->generated_slug);
      }
    }
    sync_hicolor_icon_theme();
  }

  char filepath[BUF_XL];
  snprintf(filepath, sizeof(filepath), "%s/%s.desktop", app->target_dir,
           app->generated_slug);
  FILE *f = fopen(filepath, "w");
  if (!f) {
    GtkWidget *dialog = gtk_message_dialog_new(
        window, GTK_DIALOG_MODAL, GTK_MESSAGE_ERROR, GTK_BUTTONS_OK,
        "Gagal menyimpan file .desktop!");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
    return;
  }

  fprintf(f, "[Desktop Entry]\n");
  fprintf(f, "Type=Application\n");
  fprintf(f, "Name=%s\n", app->generated_app_name);
  fprintf(f, "Exec=\"%s\"\n", app->selected_appimage);
  fprintf(f, "Icon=%s\n", final_icon_value);
  fprintf(f, "StartupWMClass=%s\n", app->generated_wm_class);
  fprintf(f, "Terminal=false\n");
  fprintf(f, "Categories=Utility;\n");
  fclose(f);
  chmod(filepath, 0755);

  char update_cmd[BUF_XL];
  snprintf(update_cmd, sizeof(update_cmd),
           "update-desktop-database \"%s\" >/dev/null 2>&1", app->target_dir);
  int res = system(update_cmd);
  (void)res;

  refresh_shortcut_list(app);

  GtkWidget *dialog = gtk_message_dialog_new(
      window, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK,
      "Shortcut berhasil dibuat!\nIkon berhasil diintegrasikan ke sistem.");
  gtk_dialog_run(GTK_DIALOG(dialog));
  gtk_widget_destroy(dialog);
}

static void on_check_icon_clicked(GtkButton *button, gpointer user_data) {
  AppWidgets *app = (AppWidgets *)user_data;
  GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));

  if (strlen(app->last_copied_icon_path) > 0 &&
      access(app->last_copied_icon_path, R_OK) == 0) {
    char msg[BUF_XL];
    snprintf(msg, sizeof(msg), "Ikon BENAR-BENAR ADA di sistem!\n\nPath:\n%s",
             app->last_copied_icon_path);
    GtkWidget *dialog = gtk_message_dialog_new(
        window, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO, GTK_BUTTONS_OK, "%s", msg);
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  } else {
    GtkWidget *dialog = gtk_message_dialog_new(
        window, GTK_DIALOG_MODAL, GTK_MESSAGE_WARNING, GTK_BUTTONS_OK,
        "Belum ada ikon yang diintegrasikan atau file tidak ditemukan.");
    gtk_dialog_run(GTK_DIALOG(dialog));
    gtk_widget_destroy(dialog);
  }
}

static void on_open_folder_clicked(GtkButton *button, gpointer user_data) {
  (void)button;
  (void)user_data;
  const char *home = getenv("HOME");
  if (home) {
    char path[BUF_LG];
    snprintf(path, sizeof(path),
             "xdg-open \"%s/.local/share/icons/hicolor/48x48/apps\" >/dev/null "
             "2>&1 &",
             home);
    int res = system(path);
    (void)res;
  }
}

static void on_delete_clicked(GtkButton *button, gpointer user_data) {
  char *filepath = (char *)g_object_get_data(G_OBJECT(button), "filepath");
  AppWidgets *app = (AppWidgets *)user_data;
  GtkWindow *window = GTK_WINDOW(gtk_widget_get_toplevel(GTK_WIDGET(button)));
  if (filepath && remove(filepath) == 0) {
    char *base = g_path_get_basename(filepath);
    char *dot = strrchr(base, '.');
    if (dot)
      *dot = '\0';
    const char *home = getenv("HOME");
    if (home) {
      int sizes[] = {48, 128, 256};
      for (int i = 0; i < 3; i++) {
        char p[BUF_LG];
        snprintf(p, sizeof(p),
                 "%s/.local/share/icons/hicolor/%dx%d/apps/%s.png", home,
                 sizes[i], sizes[i], base);
        remove(p);
      }
      char s[BUF_LG];
      snprintf(s, sizeof(s),
               "%s/.local/share/icons/hicolor/scalable/apps/%s.svg", home,
               base);
      remove(s);
      sync_hicolor_icon_theme();
    }
    g_free(base);
    char update_cmd[BUF_XL];
    snprintf(update_cmd, sizeof(update_cmd),
             "update-desktop-database \"%s\" >/dev/null 2>&1", app->target_dir);
    int res = system(update_cmd);
    (void)res;
    refresh_shortcut_list(app);
    GtkWidget *dialog =
        gtk_message_dialog_new(window, GTK_DIALOG_MODAL, GTK_MESSAGE_INFO,
                               GTK_BUTTONS_OK, "Shortcut dihapus!");
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
  if (!dir)
    return;
  struct dirent *entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, "shortcutmaker.desktop") == 0)
      continue;
    if (g_str_has_suffix(entry->d_name, ".desktop")) {
      char filepath[BUF_XL];
      snprintf(filepath, sizeof(filepath), "%s/%s", app->target_dir,
               entry->d_name);
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
      gtk_widget_set_margin_top(row_box, 4);
      gtk_widget_set_margin_bottom(row_box, 4);
      gtk_widget_set_margin_start(row_box, 6);
      gtk_widget_set_margin_end(row_box, 6);
      GtkWidget *row_icon =
          gtk_image_new_from_icon_name(icon_name, GTK_ICON_SIZE_LARGE_TOOLBAR);
      gtk_box_pack_start(GTK_BOX(row_box), row_icon, FALSE, FALSE, 0);
      GtkWidget *lbl = gtk_label_new(entry->d_name);
      gtk_widget_set_halign(lbl, GTK_ALIGN_START);
      gtk_box_pack_start(GTK_BOX(row_box), lbl, TRUE, TRUE, 0);
      GtkWidget *btn_del = gtk_button_new_with_label("Delete");
      g_object_set_data_full(G_OBJECT(btn_del), "filepath", g_strdup(filepath),
                             g_free);
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
  snprintf(app.target_dir, sizeof(app.target_dir),
           "%s/.local/share/applications", home ? home : ".");
  g_mkdir_with_parents(app.target_dir, 0755);

  GtkWidget *window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
  gtk_window_set_title(GTK_WINDOW(window),
                       "AppImage Shortcut Maker v" APP_VERSION);
  gtk_container_set_border_width(GTK_CONTAINER(window), 10);
  gtk_window_set_default_size(GTK_WINDOW(window), 550, 500);
  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);
  g_signal_connect(window, "destroy", G_CALLBACK(gtk_main_quit), NULL);

  GtkWidget *notebook = gtk_notebook_new();
  gtk_container_add(GTK_CONTAINER(window), notebook);

  GtkWidget *vbox_create = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_set_border_width(GTK_CONTAINER(vbox_create), 10);

  GtkWidget *browse_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
  app.entry_appimage = gtk_entry_new();
  gtk_editable_set_editable(GTK_EDITABLE(app.entry_appimage), FALSE);
  gtk_entry_set_placeholder_text(GTK_ENTRY(app.entry_appimage),
                                 "Pilih file .AppImage...");
  gtk_widget_set_hexpand(app.entry_appimage, TRUE);
  gtk_box_pack_start(GTK_BOX(browse_box), app.entry_appimage, TRUE, TRUE, 0);

  GtkWidget *btn_browse = gtk_button_new_with_label("Choose AppImage");
  g_signal_connect(btn_browse, "clicked", G_CALLBACK(on_browse_clicked), &app);
  gtk_box_pack_start(GTK_BOX(browse_box), btn_browse, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_create), browse_box, FALSE, FALSE, 0);

  GtkWidget *frame_icon =
      gtk_frame_new("Native Icon (Scanned from AppImage share/)");
  GtkWidget *icon_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_container_set_border_width(GTK_CONTAINER(icon_box), 8);

  app.img_preview =
      gtk_image_new_from_icon_name("applications-other", GTK_ICON_SIZE_DIALOG);
  gtk_widget_set_size_request(app.img_preview, 48, 48);
  gtk_box_pack_start(GTK_BOX(icon_box), app.img_preview, FALSE, FALSE, 0);

  app.lbl_icon_info = gtk_label_new("Belum ada file AppImage dipilih");
  gtk_widget_set_halign(app.lbl_icon_info, GTK_ALIGN_START);
  gtk_box_pack_start(GTK_BOX(icon_box), app.lbl_icon_info, TRUE, TRUE, 0);

  gtk_container_add(GTK_CONTAINER(frame_icon), icon_box);
  gtk_box_pack_start(GTK_BOX(vbox_create), frame_icon, FALSE, FALSE, 0);

  GtkWidget *frame_preview = gtk_frame_new("Preview .desktop");
  GtkWidget *scrolled_preview = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_preview),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_widget_set_size_request(scrolled_preview, -1, 110);

  app.text_preview_desktop = gtk_text_view_new();
  app.preview_buffer =
      gtk_text_view_get_buffer(GTK_TEXT_VIEW(app.text_preview_desktop));
  gtk_text_view_set_editable(GTK_TEXT_VIEW(app.text_preview_desktop), FALSE);
  gtk_text_view_set_cursor_visible(GTK_TEXT_VIEW(app.text_preview_desktop),
                                   FALSE);

  GtkCssProvider *css_provider = gtk_css_provider_new();
  gtk_css_provider_load_from_data(
      css_provider, "textview { font-family: monospace; font-size: 9pt; }", -1,
      NULL);
  gtk_style_context_add_provider(
      gtk_widget_get_style_context(app.text_preview_desktop),
      GTK_STYLE_PROVIDER(css_provider),
      GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  g_object_unref(css_provider);

  gtk_text_buffer_set_text(app.preview_buffer,
                           "# Preview .desktop akan tampil di sini...", -1);

  gtk_container_add(GTK_CONTAINER(scrolled_preview), app.text_preview_desktop);
  gtk_container_add(GTK_CONTAINER(frame_preview), scrolled_preview);
  gtk_box_pack_start(GTK_BOX(vbox_create), frame_preview, FALSE, FALSE, 0);

  GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);

  GtkWidget *btn_create =
      gtk_button_new_with_label("Create Shortcut & Integrate Icon");
  gtk_widget_set_size_request(btn_create, -1, 38);
  g_signal_connect(btn_create, "clicked",
                   G_CALLBACK(on_create_shortcut_clicked), &app);
  gtk_box_pack_start(GTK_BOX(action_box), btn_create, TRUE, TRUE, 0);

  GtkWidget *sub_action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);

  GtkWidget *btn_check_icon = gtk_button_new_with_label("Check Icon in System");
  g_signal_connect(btn_check_icon, "clicked", G_CALLBACK(on_check_icon_clicked),
                   &app);
  gtk_widget_set_hexpand(btn_check_icon, TRUE);
  gtk_box_pack_start(GTK_BOX(sub_action_box), btn_check_icon, TRUE, TRUE, 0);

  GtkWidget *btn_open_folder = gtk_button_new_with_label("Open Icon Folder");
  g_signal_connect(btn_open_folder, "clicked",
                   G_CALLBACK(on_open_folder_clicked), &app);
  gtk_widget_set_hexpand(btn_open_folder, TRUE);
  gtk_box_pack_start(GTK_BOX(sub_action_box), btn_open_folder, TRUE, TRUE, 0);

  gtk_box_pack_start(GTK_BOX(action_box), sub_action_box, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(vbox_create), action_box, FALSE, FALSE, 0);

  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox_create,
                           gtk_label_new("Create Shortcut"));

  GtkWidget *vbox_manage = gtk_box_new(GTK_ORIENTATION_VERTICAL, 10);
  gtk_container_set_border_width(GTK_CONTAINER(vbox_manage), 8);
  GtkWidget *scrolled_window = gtk_scrolled_window_new(NULL, NULL);
  gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(scrolled_window),
                                 GTK_POLICY_AUTOMATIC, GTK_POLICY_AUTOMATIC);
  gtk_box_pack_start(GTK_BOX(vbox_manage), scrolled_window, TRUE, TRUE, 0);
  app.list_box = gtk_list_box_new();
  gtk_container_add(GTK_CONTAINER(scrolled_window), app.list_box);
  gtk_notebook_append_page(GTK_NOTEBOOK(notebook), vbox_manage,
                           gtk_label_new("Manage Shortcuts"));

  refresh_shortcut_list(&app);
  gtk_widget_show_all(window);
  gtk_main();
  return 0;
}
