/*
  This file is not licenced under the GPL like the rest of the code.
  Its is under the MIT license, to encourage reuse by cut-and-paste.

  Copyright (c) 2007 Red Hat, Inc.

  Permission is hereby granted, free of charge, to any person
  obtaining a copy of this software and associated documentation files
  (the "Software"), to deal in the Software without restriction,
  including without limitation the rights to use, copy, modify, merge,
  publish, distribute, sublicense, and/or sell copies of the Software,
  and to permit persons to whom the Software is furnished to do so,
  subject to the following conditions: 

  The above copyright notice and this permission notice shall be
  included in all copies or substantial portions of the Software. 

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
  EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
  MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
  NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
  BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
  ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
  CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>

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

/**
 * xdg_user_dir_lookup_with_fallback:
 * @type: a string specifying the type of directory
 * @fallback: value to use if the directory isn't specified by the user
 * @returns: a newly allocated absolute pathname
 *
 * Looks up a XDG user directory of the specified type.
 * Example of types are "DESKTOP" and "DOWNLOAD".
 *
 * In case the user hasn't specified any directory for the specified
 * type the value returned is @fallback.
 *
 * The return value is newly allocated and must be freed with
 * free(). The return value is never NULL if @fallback != NULL, unless
 * out of memory.
 **/
static char *
xdg_user_dir_lookup_with_fallback (const char *type, const char *fallback)
{
  const char *home_dir, *config_home;
  char *config_file;
  char *buffer;
  char *user_dir;
  char *key, *key_end;
  char *value, *value_end;
  char *p;
  char **lines;
  int idx;
  gboolean relative;
  gboolean res;
  
  home_dir = g_get_home_dir ();
  if (home_dir == NULL)
    goto error;

  config_home = g_get_user_config_dir ();
  config_file = g_build_filename (config_home, "user-dirs.dirs", NULL);
  res = g_file_get_contents (config_file, &buffer, NULL, NULL);
  g_free (config_file);

  if (!res)
    goto error;

  user_dir = NULL;
  lines = g_strsplit (buffer, "\n", -1);
  g_free (buffer);

  for (idx = 0; lines[idx] != NULL; idx++)
    {
      p = lines[idx];
      while (g_ascii_isspace (*p))
	p++;

      key = p;
      while (*p && !g_ascii_isspace (*p) && * p != '=')
	p++;

      if (*p == 0)
	continue;

      key_end = p++;
      key = user_dirs_key_from_string (key, key_end - key);
      if (g_strcmp0 (key, type) != 0)
        continue;

      while (g_ascii_isspace (*p))
	p++;

      if (*p != '"')
	continue;
      p++;
      
      relative = FALSE;
      if (g_str_has_prefix (p, "$HOME/"))
	{
	  p += 6;
	  relative = TRUE;
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
      
      if (relative)
        user_dir = g_build_filename (home_dir, value, NULL);
      else
        user_dir = g_strdup (value);

      break;
    }
  g_strfreev (lines);

  if (user_dir)
    return user_dir;

 error:
  if (fallback)
    return strdup (fallback);
  return NULL;
}

/**
 * xdg_user_dir_lookup:
 * @type: a string specifying the type of directory
 * @returns: a newly allocated absolute pathname
 *
 * Looks up a XDG user directory of the specified type.
 * Example of types are "DESKTOP" and "DOWNLOAD".
 *
 * The return value is always != NULL (unless out of memory),
 * and if a directory
 * for the type is not specified by the user the default
 * is the home directory. Except for DESKTOP which defaults
 * to ~/Desktop.
 *
 * The return value is newly allocated and must be freed with
 * free().
 **/
static char *
xdg_user_dir_lookup (const char *type)
{
  char *dir;
  const char *home_dir;
	  
  dir = xdg_user_dir_lookup_with_fallback (type, NULL);
  if (dir != NULL)
    return dir;

  home_dir = g_get_home_dir ();
  if (home_dir == NULL)
    return strdup ("/tmp");
  
  /* Special case desktop for historical compatibility */
  if (strcmp (type, "DESKTOP") == 0)
    return g_build_filename (home_dir, "Desktop", NULL);

  return strdup (home_dir);
}

#ifdef STANDALONE_XDG_USER_DIR_LOOKUP
int
main (int argc, char *argv[])
{
  if (argc != 2)
    {
      g_printerr ("Usage %s <dir-type>\n", argv[0]);
      exit (1);
    }
  
  printf ("%s\n", xdg_user_dir_lookup (argv[1]));
  return 0;
}
#endif
