#include <config.h>

#define _GNU_SOURCE

#include <sys/types.h>
#include <sys/stat.h>
#include <libintl.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdarg.h>
#include <errno.h>
#include <iconv.h>
#include <langinfo.h>
#include <glib.h>
#include <glib/gstdio.h>

typedef struct {
  char *name;
  char *path;
} Directory;

Directory backwards_compat_dirs[] = {
  { "DESKTOP", "Desktop" },
  { "TEMPLATES", "Templates" },
  { "PUBLICSHARE", "Public" },
  { NULL}
};

static GList *default_dirs = NULL;
static GList *user_dirs = NULL;

/* Config: */
static gboolean conf_enabled = TRUE;
static char *conf_filename_encoding = NULL; /* NULL => utf8 */

static iconv_t filename_converter = (iconv_t)(-1);

/* Args */
static char *arg_dummy_file = NULL;
static char *arg_set_dir = NULL;
static char *arg_set_value = NULL;
static gboolean arg_force = FALSE;
static gboolean arg_move = FALSE;

static Directory *
directory_new (const char *name, const char *path)
{
  Directory *dir;
  dir = g_new0 (Directory, 1);
  dir->name = g_strdup (name);
  dir->path = g_strdup (path);
  return dir;
}

static void
remove_trailing_whitespace (char *s)
{
  int len;

  len = strlen (s);
  while (len > 0 && g_ascii_isspace (s[len-1]))
    {
      s[len-1] = 0;
      len--;
    }
}

static char *
shell_unescape (char *escaped)
{
  char *unescaped;
  char *d;

  unescaped = g_malloc (strlen (escaped) + 1);

  d = unescaped;

  while (*escaped)
    {
      if (*escaped == '\\' && *(escaped + 1) != 0)
	escaped++;
      *d++ = *escaped++;
    }
  *d = 0;
  return unescaped;
}

static char *
shell_escape (char *unescaped)
{
  char *escaped;
  char *d;

  escaped = g_malloc (strlen (unescaped) * 2 + 1);

  d = escaped;

  while (*unescaped)
    {
      if (*unescaped == '$' ||
	  *unescaped == '`' ||
	  *unescaped == '\\')
	*d++ = '\\';
      *d++ = *unescaped++;
    }
  *d = 0;
  return escaped;
}

static char *
filename_from_utf8 (const char *utf8_path)
{
  size_t res, len;
  const char *in;
  char *out, *outp;
  size_t in_left, out_left, outbuf_size;
  int done;
  
  if (filename_converter == (iconv_t)(-1))
    return strdup (utf8_path);

  len = strlen (utf8_path);
  outbuf_size = len + 1;

  done = 0;
  do
    {
      in = utf8_path;
      in_left = len;
      out = malloc (outbuf_size);
      out_left = outbuf_size - 1;
      outp = out;
  
      res = iconv (filename_converter,
		   (ICONV_CONST char **)&in, &in_left,
		   &outp, &out_left);
      if (res == (size_t)(-1) &&  errno == E2BIG)
	{
	  free (out);
	  outbuf_size *= 2;
	}
      else
	done = 1;
    }
  while (!done);

  if (res == (size_t)(-1))
    {
      free (out);
      return NULL;
    }

  /* zero terminate */
  *outp = 0;
  return out;
}

static char *
get_user_config_file (const char *filename)
{
  const gchar *config_home;
  config_home = g_get_user_config_dir ();
  return g_build_filename (config_home, filename, NULL);
}

static GList *
get_config_files (char *filename)
{
  int i;
  char *file;
  const char * const *config_paths;
  GList *paths;

  paths = NULL;

  file = get_user_config_file (filename);
  if (file)
    {
      if (g_file_test (file, G_FILE_TEST_IS_REGULAR))
        paths = g_list_prepend (paths, file);
      else
	g_free (file);
    }

  config_paths = g_get_system_config_dirs ();
  for (i = 0; config_paths[i] != NULL; i++)
    {
      file = g_build_filename (config_paths[i], filename, NULL);
      if (g_file_test (file, G_FILE_TEST_IS_REGULAR))
        paths = g_list_prepend (paths, file);
      else
	g_free (file);
    }
  
  return g_list_reverse (paths);
}

static gboolean
is_true (const char *str)
{
  while (g_ascii_isspace (*str))
    str++;
  
  if (*str == '1' ||
      g_str_has_prefix (str, "True") ||
      g_str_has_prefix (str, "true"))
    return TRUE;
  return FALSE;
}

static void
load_config (char *path)
{
  char *buffer, *p;
  char *encoding;
  char **lines;
  int idx;
  gboolean res;

  res = g_file_get_contents (path, &buffer, NULL, NULL);
  if (!res)
    return;

  lines = g_strsplit (buffer, "\n", -1);
  g_free (buffer);

  for (idx = 0; lines[idx] != NULL; idx++)
    {
      p = lines[idx];

      /* Skip whitespace */
      while (g_ascii_isspace (*p))
	p++;

      /* Skip comment lines */      
      if (*p == '#')
	continue;

      remove_trailing_whitespace (p);

      if (g_str_has_prefix (p, "enabled="))
	{
	  p += strlen ("enabled=");
	  conf_enabled = is_true (p);
	}
      if (g_str_has_prefix (p, "filename_encoding="))
	{
	  p += strlen ("filename_encoding=");

	  while (g_ascii_isspace (*p))
	    p++;

          remove_trailing_whitespace (p);  
          encoding = g_ascii_strup (p, -1);
          g_free (conf_filename_encoding);
  
	  if (strcmp (encoding, "UTF8") == 0 ||
	      strcmp (encoding, "UTF-8") == 0)
	    conf_filename_encoding = NULL;
	  else if (strcmp (encoding, "LOCALE") == 0)
	    conf_filename_encoding = g_strdup (nl_langinfo (CODESET));
	  else
	    conf_filename_encoding = g_strdup (encoding);

          g_free (encoding);
	}
    }

  g_strfreev (lines);
}

static gboolean
load_all_configs (void)
{
  GList *paths, *l;
  
  paths = get_config_files ("user-dirs.conf");

  /* Load config files in reverse */
  for (l = g_list_last (paths); l != NULL; l = l->prev)
    load_config (l->data);

  g_list_foreach (paths, (GFunc) g_free, NULL);
  g_list_free (paths);

  if (conf_filename_encoding)
    {
      filename_converter = iconv_open (conf_filename_encoding, "UTF-8");
      if (filename_converter == (iconv_t)(-1))
	{
	  g_printerr ("Can't convert from UTF-8 to %s\n", conf_filename_encoding);
	  return FALSE;
	}
    }

  return TRUE;
}

static int
compare_dir_name (const Directory *dir, const char *name)
{
  return strcmp (dir->name, name);
}

static Directory *
find_dir (GList *dirs, const char *name)
{
  GList *l;
  l = g_list_find_custom (dirs, name, (GCompareFunc) compare_dir_name);
  return (l != NULL) ? l->data : NULL;
}

/* modifies the input string */
static char *
user_dirs_key_from_string (char *string,
                           int len)
{
  if (len < 0)
    len = strlen (string);

  string[len] = '\0';

  if (g_str_has_suffix (string, ".desktop"))
    return string;

  if (g_str_has_prefix (string, "XDG_") &&
      g_str_has_suffix (string, "_DIR"))
    {
      string[len - 4] = '\0';
      return string + 4;
    }

  return NULL;
}

static char *
user_dirs_key_to_string (char *key)
{
  if (g_str_has_suffix (key, ".desktop"))
    return g_strdup (key);

  return g_strdup_printf ("XDG_%s_DIR", key);
}

static Directory *
get_dir_for_desktop_file (char *desktop_file_path)
{
  GKeyFile *keyfile;
  char *special_dir_path;
  Directory *retval;
  char *desktop_id;
  char *parent_name, *parent_val;
  Directory *parent_dir;
  char *translated_name;
  gboolean res;

  keyfile = g_key_file_new ();
  desktop_id = g_path_get_basename (desktop_file_path);
  special_dir_path = NULL;

  res = g_key_file_load_from_file (keyfile, desktop_file_path,
                                   G_KEY_FILE_NONE,
                                   NULL);
  if (!res)
    goto out;

  parent_val = g_key_file_get_string (keyfile,
                                      G_KEY_FILE_DESKTOP_TYPE_DIRECTORY,
                                      "Parent",
                                      NULL);
  if (!parent_val)
    goto out;

  parent_name = user_dirs_key_from_string (parent_val, -1);
  if (!parent_name)
    goto out;

  parent_dir = find_dir (default_dirs, parent_name);
  if (!parent_dir)
    goto out;

  translated_name = g_key_file_get_locale_string (keyfile,
                                                  G_KEY_FILE_DESKTOP_TYPE_DIRECTORY,
                                                  G_KEY_FILE_DESKTOP_KEY_NAME,
                                                  NULL, NULL);
  if (!translated_name)
    goto out;

  special_dir_path = g_build_filename (parent_dir->path, translated_name, NULL);
  g_key_file_free (keyfile);

 out:
  if (special_dir_path != NULL)
    retval = directory_new (desktop_id, special_dir_path);
  else
    retval = NULL;

  g_free (desktop_id);
  g_free (special_dir_path);
  return retval;
}

static GList *
load_default_application_dirs (void)
{
  const char * const * data_paths;
  GList *app_dirs = NULL;
  int idx;

  data_paths = g_get_system_data_dirs ();

  for (idx = 0; data_paths[idx] != NULL; idx++)
    {
      char *path;
      GDir *dir;
      const gchar *basename;

      path = g_build_filename (data_paths[idx], "xdg-user-dirs", NULL);
      if (!g_file_test (path, G_FILE_TEST_IS_DIR))
        {
          g_free (path);
          continue;
        }

      dir = g_dir_open (path, 0, NULL);
      if (!dir)
        {
          g_free (path);
          continue;
        }

      while ((basename = g_dir_read_name (dir)) != NULL)
        {
          Directory *new_dir;
          char *desktop_file_path;

          if (!g_str_has_suffix (basename, ".desktop"))
            continue;

          if (find_dir (app_dirs, basename))
            continue;

          desktop_file_path = g_build_filename (path, basename, NULL);
          new_dir = get_dir_for_desktop_file (desktop_file_path);

          if (new_dir != NULL)
            app_dirs = g_list_prepend (app_dirs, new_dir);

          g_free (desktop_file_path);
        }
      
      g_free (path);
      g_dir_close (dir);
    }

  return app_dirs;
}

static gboolean
load_default_dirs (void)
{
  char *buffer, *p;
  char *key, *key_end, *value;
  char **lines;
  int idx;
  Directory *dir;
  GList *paths;
  gboolean res;

  res = FALSE;
  paths = get_config_files ("user-dirs.defaults");
  if (paths == NULL)
    {
      g_printerr ("No default user directories\n");
      goto out;
    }

  res = g_file_get_contents (paths->data, &buffer, NULL, NULL);  
  if (!res)
    {
      g_printerr ("Can't open %s\n", (char *) paths->data);
      goto out;
    }

  lines = g_strsplit (buffer, "\n", -1);
  g_free (buffer);

  for (idx = 0; lines[idx] != NULL; idx++)
    {
      p = lines[idx];

      /* Skip whitespace */
      while (g_ascii_isspace (*p))
	p++;

      /* Skip comment lines */
      if (*p == '#')
	continue;

      key = p;
      while (*p && !g_ascii_isspace (*p) && * p != '=')
	p++;

      key_end = p;

      while (g_ascii_isspace (*p))
	p++;
      if (*p == '=')
	p++;
      while (g_ascii_isspace (*p))
	p++;
      
      value = p;

      *key_end = 0;

      if (*key == 0 || *value == 0)
	continue;
      
      dir = directory_new (key, value);
      default_dirs = g_list_prepend (default_dirs, dir);
    }

  g_strfreev (lines);

 out:
  g_list_foreach (paths, (GFunc) g_free, NULL);
  g_list_free (paths);

  /* now load default application-provided dirs */
  default_dirs = g_list_concat (default_dirs, load_default_application_dirs ());
  
  return res;
}

static void
load_user_dirs (void)
{
  char *buffer, *p;
  char *key, *key_end, *value, *value_end;
  char *unescaped;
  char **lines;
  int idx;
  Directory *dir;
  char *user_config_file;
  gboolean res;

  user_config_file = get_user_config_file ("user-dirs.dirs");
  res = g_file_get_contents (user_config_file, &buffer, NULL, NULL);
  g_free (user_config_file);

  if (!res)
    return;

  lines = g_strsplit (buffer, "\n", -1);
  g_free (buffer);

  for (idx = 0; lines[idx] != NULL; idx++)
    {
      p = lines[idx];

      /* Skip whitespace */
      while (g_ascii_isspace (*p))
	p++;

      /* Skip comment lines */
      if (*p == '#')
	continue;

      key = p;
      while (*p && !g_ascii_isspace (*p) && * p != '=')
	p++;

      if (*p == 0)
	continue;

      key_end = p++;
      key = user_dirs_key_from_string (key, key_end - key);
      if (key == NULL)
        continue;

      while (g_ascii_isspace (*p))
        p++;
      if (*p == '=')
	p++;
      while (g_ascii_isspace (*p))
	p++;

      if (*p++ != '"')
	continue;	

      if (g_str_has_prefix (p, "$HOME"))
	{
	  p += 5;
	  if (*p == '/')
	    p++;
	  else if (*p != '"' && *p != 0)
	    continue; /* Not ending after $HOME, nor followed by slash. Ignore */
	}
      else if (*p != '/')
	continue;
      value = p;

      while (*p)
	{
	  if (*p == '"')
	    break;
	  if (*p == '\\' && *(p+1) != 0)
	    p++;

	  p++;
	}

      value_end = p;
      *value_end = 0;

      unescaped = shell_unescape (value);
      dir = directory_new (key, unescaped);
      user_dirs = g_list_prepend (user_dirs, dir);
      g_free (unescaped);      
    }

  user_dirs = g_list_reverse (user_dirs);
  g_strfreev (lines);
}

static void
save_locale (void)
{
  char *user_locale_file;
  char *locale, *dot;

  user_locale_file = get_user_config_file ("user-dirs.locale");
  
  locale = g_strdup (setlocale (LC_MESSAGES, NULL));
  /* Skip encoding part */
  dot = strchr (locale, '.');
  if (dot)
    *dot = 0;

  if (!g_file_set_contents (user_locale_file, locale, -1, NULL))
    g_printerr ("Can't save user-dirs.locale\n");

  g_free (user_locale_file);
  g_free (locale);
}

static gboolean
save_user_dirs (const char *dummy_file)
{
  FILE *file;
  char *user_config_file;
  char *tmp_file;
  GList *l;
  Directory *user_dir;
  int tmp_fd;
  gboolean res;
  char *dir;

  res = TRUE;

  tmp_file = NULL;
  if (dummy_file)
    user_config_file = g_strdup (dummy_file);
  else
    user_config_file = get_user_config_file ("user-dirs.dirs");

  dir = g_path_get_dirname (user_config_file);  
  if (g_mkdir_with_parents (dir, 0700) < 0)
    {
      g_printerr ("Can't save user-dirs.dirs, failed to create directory\n");
      res = FALSE;
      goto out;
    }

  tmp_file = g_strconcat (user_config_file, "XXXXXX", NULL);  
  tmp_fd = mkstemp (tmp_file);
  if (tmp_fd == -1)
    {
      g_printerr ("Can't save user-dirs.dirs\n");
      res = FALSE;
      goto out;
    }
  
  file = fdopen (tmp_fd, "w");
  if (file == NULL)
    {
      unlink (tmp_file);
      g_printerr ("Can't save user-dirs.dirs\n");
      res = FALSE;
      goto out;
    }

  fprintf (file, "# This file is written by xdg-user-dirs-update\n");
  fprintf (file, "# If you want to change or add directories, just edit the line you're\n");
  fprintf (file, "# interested in. All local changes will be retained on the next run\n");
  fprintf (file, "# Format for general directories is XDG_xxx_DIR=\"$HOME/yyy\", where yyy is a shell-escaped\n");
  fprintf (file, "# homedir-relative path, or XDG_xxx_DIR=\"/yyy\", where /yyy is an\n");
  fprintf (file, "# absolute path.\n");
  fprintf (file, "# Format for desktop-file speficic directories is\n");
  fprintf (file, "# xxx.desktop=\"yyy\" where xxx.desktop is a valid directory\"\n");
  fprintf (file, "# keyfile in $XDG_DATA_DIRS/xdg-user-dirs.\n");
  fprintf (file, "# No other format is supported.\n");
  fprintf (file, "# \n");

  for (l = user_dirs; l != NULL; l = l->next)
    {
      char *escaped, *name;
      const char *relative_prefix;

      user_dir = l->data;

      name = user_dirs_key_to_string (user_dir->name);
      escaped = shell_escape (user_dir->path);
      if (g_path_is_absolute (escaped))
        relative_prefix = "";
      else
        relative_prefix = "$HOME/";

      fprintf (file, "%s=\"%s%s\"\n",
               name,
               relative_prefix,
               escaped);
      g_free (escaped);
      g_free (name);
    }

  fclose (file);

  if (rename (tmp_file, user_config_file) == -1)
    {
      unlink (tmp_file);
      g_printerr ("Can't save user-dirs.dirs\n");
      res = FALSE;
    }

 out:
  g_free (dir);
  g_free (tmp_file);
  g_free (user_config_file);
  return res;
}


static char *
localize_path_name (const char *path)
{
  char *res;
  const char *element, *element_end;
  char *element_copy;
  char *translated;
  gboolean has_slash;

  res = g_strdup ("");

  while (*path)
    {
      has_slash = FALSE;
      while (*path == '/')
	{
	  path++;
	  has_slash = TRUE;
	}

      element = path;
      while (*path && *path != '/')
	path++;
      element_end = path;

      element_copy = g_strndup (element, element_end - element);
      translated = gettext (element_copy);

      res = g_realloc (res, strlen (res) + 1 + strlen (translated) + 1);
      if (has_slash)
	strcat (res, "/");
      strcat (res, translated);
      
      g_free (element_copy);
    }
  
  return res;
}

static char *
make_path_absolute (const char *path)
{
  if (g_path_is_absolute (path))
    return g_strdup (path);
  else
    return g_build_filename (g_get_home_dir (), path, NULL);
}

static gboolean
validate_user_dir_path (Directory *user_dir)
{
  char *path_name;
  gboolean path_valid = TRUE;

  path_name = make_path_absolute (user_dir->path);

  /* If the path doesn't exist, reset it to an empty value.
   * By spec, it will be treated as the home directory itself.
   */
  if (!g_file_test (path_name, G_FILE_TEST_IS_DIR))
    {
      g_printerr ("%s was removed, reassigning %s to homedir\n",
                  path_name, user_dir->name);
      g_free (user_dir->path);
      user_dir->path = g_strdup ("");
      path_valid = FALSE;
    }
 
  g_free (path_name);
  return path_valid;
}

static char *
get_backwards_compat_path (Directory *default_dir,
                           char **relative_path_name_out)
{
  Directory *compat_dir;
  char *path_name, *relative_path_name;
  int idx;

  path_name = NULL;
  relative_path_name = NULL;
  compat_dir = NULL;

  for (idx = 0; backwards_compat_dirs[idx].name != NULL; idx++)
    {
      if (compare_dir_name (default_dir, backwards_compat_dirs[idx].name) == 0)
        {
          compat_dir = &backwards_compat_dirs[idx];
          break;
        }
    }

  if (compat_dir)
    {
      path_name = g_build_filename (g_get_home_dir (), compat_dir->path, NULL);
      if (g_file_test (path_name, G_FILE_TEST_IS_DIR))
        {
          relative_path_name = g_strdup (compat_dir->path);
        }
      else
        {
          g_free (path_name);
          path_name = NULL;
        }
    }

  if (relative_path_name_out != NULL)
    *relative_path_name_out = relative_path_name;
  else
    g_free (relative_path_name);

  return path_name;
}

static char *
get_translated_path_name (Directory *default_dir,
                          char **relative_path_name_out)
{
  char *path_name, *relative_path_name, *translated_name;

  translated_name = localize_path_name (default_dir->path);
  relative_path_name = filename_from_utf8 (translated_name);

  if (relative_path_name == NULL)
    relative_path_name = g_strdup (translated_name);
  g_free (translated_name);

  path_name = make_path_absolute (relative_path_name);

  if (relative_path_name_out != NULL)
    *relative_path_name_out = relative_path_name;
  else
    g_free (relative_path_name);

  return path_name;
}

static void
update_user_dirs_path (const char *old_path,
                       const char *new_path)
{
  GList *l;
  Directory *user_dir;
  const char *p;
  char *new_full_path;

  for (l = user_dirs; l != NULL; l = l->next)
    {
      user_dir = l->data;
      if (!g_str_has_prefix (user_dir->path, old_path))
        continue;

      p = user_dir->path + strlen (old_path);
      new_full_path = g_build_filename (new_path, p, NULL);
      g_free (user_dir->path);
      user_dir->path = new_full_path;
    }
}

static int
default_dirs_compare (gconstpointer a,
                      gconstpointer b)
{
  Directory *dir_a = (Directory *) a;
  Directory *dir_b = (Directory *) b;

  /* The second directory is first's parent,
   * so sort it before.
   */
  if (g_str_has_prefix (dir_a->path, dir_b->path))
    return 1;

  /* The first directory is second's parent,
   * so sort it before.
   */
  if (g_str_has_prefix (dir_b->path, dir_a->path))
    return -1;

  return g_utf8_collate (dir_a->path, dir_b->path);
}

static gboolean
create_default_dirs (gboolean force, gboolean for_dummy_file)
{
  GList *sorted_dirs, *l;
  Directory *user_dir, *default_dir;
  char *old_relative_path_name, *path_name, *relative_path_name;
  gboolean user_dirs_changed = FALSE;

  /* Sort directories so that parent dirs come first than their children.
   * This makes it easier to move subdirectories - see comment below.
   */
  sorted_dirs = g_list_sort (default_dirs, default_dirs_compare);

  for (l = sorted_dirs; l != NULL; l = l->next)
    {
      default_dir = l->data;
      user_dir = find_dir (user_dirs, default_dir->name);

      if (user_dir != NULL && !force)
        {
          /* If we found an user dir for this default dir,
           * don't re-create it, but make sure to validate its
           * path first.
           */
          user_dirs_changed |= !validate_user_dir_path (user_dir);
          continue;
        }

      old_relative_path_name = NULL;
      path_name = NULL;
      relative_path_name = NULL;

      if (user_dir == NULL && !force)
        {
          /* New default dir. Check if its an old named dir. We want to
           * reuse that if it exists.
           */
          path_name = get_backwards_compat_path (default_dir, &relative_path_name);
        }

      if (path_name == NULL)
        {
          /* Get the default translated path name for this dir */
          path_name = get_translated_path_name (default_dir, &relative_path_name);
        }

      if (user_dir != NULL)
        old_relative_path_name = g_strdup (user_dir->path);

      if (g_strcmp0 (relative_path_name, old_relative_path_name) != 0)
        {
          gint res = 0;

	  /* Don't touch directories if we're writing a dummy output file */
          if (!for_dummy_file)
            {
              res = g_mkdir_with_parents (path_name, 0755);
              if (res >= 0 && arg_move && (old_relative_path_name != NULL))
                {
                  char *old_path_name;

                  old_path_name = make_path_absolute (old_relative_path_name);
                  if (g_file_test (old_path_name, G_FILE_TEST_EXISTS))
                    {
                      res = g_rename (old_path_name, path_name);
                      g_free (old_path_name);
                    }
                }
            }

          if (res < 0 && errno != EEXIST && errno != ENOTEMPTY)
            {
              g_free (old_relative_path_name);
              g_free (relative_path_name);
              g_free (path_name);
              continue;
            }

          user_dirs_changed = TRUE;
          if (user_dir == NULL)
            {
              /* This is a new directory altogether */
              printf ("Creating new directory %s for %s\n", default_dir->name, relative_path_name);
              user_dir = directory_new (default_dir->name, relative_path_name);
              user_dirs = g_list_append (user_dirs, user_dir);
            }
          else
            {
              /* We forced an update */
              printf ("Moving %s directory from %s to %s\n",
                      default_dir->name, old_relative_path_name, relative_path_name);
              g_free (user_dir->path);
              user_dir->path = g_strdup (relative_path_name);
            }

          /* Now update all the other paths that contain the old
           * path to the one we just renamed to
           */
          if (old_relative_path_name != NULL)
            update_user_dirs_path (old_relative_path_name, relative_path_name);
        }

      g_free (old_relative_path_name);
      g_free (relative_path_name);
      g_free (path_name);
    }

  g_list_free (sorted_dirs);

  return user_dirs_changed;
}

static gboolean
set_one_directory (const char *set_dir, const char *set_value)
{
  Directory *dir;
  char *path;
  const gchar *home;
  /* Set a key */

  home = g_get_home_dir ();

  path = (char *) set_value;
  if (g_str_has_prefix (path, home))
    {
      path += strlen (home);
      while (*path == '/')
        path++;
    }

  dir = find_dir (user_dirs, set_dir);
  if (dir != NULL)
    {
      g_free (dir->path);
      dir->path = g_strdup (path);
    }
  else
    {
      Directory *new_dir;

      new_dir = directory_new (set_dir, path);
      user_dirs = g_list_append (user_dirs, new_dir);
    }

  return save_user_dirs (arg_dummy_file);
}

static void
parse_argv (int argc, char *argv[])
{
  int i;

  for (i = 1; i < argc; i++)
    {
      if (strcmp (argv[i], "--help") == 0)
        {
          printf ("Usage: xdg-user-dirs-update [--force] [--move] [--dummy-output <path>] [--set DIR path]\n");
          exit (0);
        }
      else if (strcmp (argv[i], "--force") == 0)
        arg_force = TRUE;
      else if (strcmp (argv[i], "--move") == 0)
        arg_move = TRUE;
      else if (strcmp (argv[i], "--dummy-output") == 0 && i + 1 < argc)
        arg_dummy_file = argv[++i];
      else if (strcmp (argv[i], "--set") == 0 && i + 2 < argc)
        {
          arg_set_dir = argv[++i];
          arg_set_value = argv[++i];

          if (!g_path_is_absolute (arg_set_value))
            {
              printf ("directory value must be absolute path (was %s)\n", arg_set_value);
              exit (1);
            }
        }
      else
        {
          printf ("Invalid argument %s\n", argv[i]);
          exit (1);
        }
    }
}

static void
init_locale (void)
{
  char *locale_dir = NULL;

  setlocale (LC_ALL, "");
  
  if (g_file_test (LOCALEDIR, G_FILE_TEST_IS_DIR))
    {
      locale_dir = g_strdup (LOCALEDIR);
    }
  else
    {
      /* In case LOCALEDIR does not exist, e.x. xdg-user-dirs is installed in
       * a different location than the one determined at compile time, look
       * through the XDG_DATA_DIRS environment variable for alternate locations
       * of the locale files */
      const char * const * data_paths;
      int i;

      data_paths = g_get_system_data_dirs ();
      for (i = 0; data_paths[i] != NULL; i++)
        {
          char *dir = NULL;
          dir = g_build_filename (data_paths[i], "locale", NULL);
          if (g_file_test (dir, G_FILE_TEST_IS_DIR))
            {
              locale_dir = dir;
              break;
            }

          g_free (dir);
        }
    }

  bindtextdomain (GETTEXT_PACKAGE, locale_dir);
  g_free (locale_dir);

  bind_textdomain_codeset (GETTEXT_PACKAGE, "UTF-8");
  textdomain (GETTEXT_PACKAGE);
}

int
main (int argc, char *argv[])
{
  gboolean was_empty, user_dirs_changed;
  
  init_locale ();
  parse_argv (argc, argv);

  if (!load_all_configs ())
    return 1;

  load_user_dirs ();

  if (arg_set_dir != NULL)
    return !set_one_directory (arg_set_dir, arg_set_value);

  /* default: update */
  if (!conf_enabled)
    return 0;

  if (!load_default_dirs ())
    return 1;
      
  was_empty = (user_dirs == NULL);
  user_dirs_changed = create_default_dirs (arg_force, (arg_dummy_file != NULL));

  if (user_dirs_changed)
    {
      if (!save_user_dirs (arg_dummy_file))
        return 1;
	  
      if ((arg_force || was_empty) && arg_dummy_file == NULL)
        save_locale ();
    }

  return 0;
}
