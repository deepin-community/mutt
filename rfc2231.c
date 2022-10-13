/*
 * Copyright (C) 1999-2001 Thomas Roessler <roessler@does-not-exist.org>
 *
 *     This program is free software; you can redistribute it
 *     and/or modify it under the terms of the GNU General Public
 *     License as published by the Free Software Foundation; either
 *     version 2 of the License, or (at your option) any later
 *     version.
 *
 *     This program is distributed in the hope that it will be
 *     useful, but WITHOUT ANY WARRANTY; without even the implied
 *     warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *     PURPOSE.  See the GNU General Public License for more
 *     details.
 *
 *     You should have received a copy of the GNU General Public
 *     License along with this program; if not, write to the Free
 *     Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *     Boston, MA  02110-1301, USA.
 */

/*
 * Yet another MIME encoding for header data.  This time, it's
 * parameters, specified in RFC 2231, and modeled after the
 * encoding used in URLs.
 *
 * Additionally, continuations and encoding are mixed in an, errrm,
 * interesting manner.
 *
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "mime.h"
#include "charset.h"
#include "rfc2047.h"
#include "rfc2231.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>

struct rfc2231_parameter
{
  char *attribute;
  char *value;
  int  index;
  int  encoded;
  struct rfc2231_parameter *next;
};

static char *rfc2231_get_charset (char *, char *, size_t);
static struct rfc2231_parameter *rfc2231_new_parameter (void);
static void rfc2231_decode_one (char *, char *);
static void rfc2231_free_parameter (struct rfc2231_parameter **);
static void rfc2231_join_continuations (PARAMETER **, struct rfc2231_parameter *);
static void rfc2231_list_insert (struct rfc2231_parameter **, struct rfc2231_parameter *);

static void purge_empty_parameters (PARAMETER **headp)
{
  PARAMETER *p, *q, **last;

  for (last = headp, p = *headp; p; p = q)
  {
    q = p->next;
    if (!p->attribute || !p->value)
    {
      *last = q;
      p->next = NULL;
      mutt_free_parameter (&p);
    }
    else
      last = &p->next;
  }
}


void rfc2231_decode_parameters (PARAMETER **headp)
{
  PARAMETER *head = NULL;
  PARAMETER **last;
  PARAMETER *p, *q;

  struct rfc2231_parameter *conthead = NULL;
  struct rfc2231_parameter *conttmp;

  char *s, *t;
  char charset[STRING];

  int encoded;
  int index;
  short dirty = 0;	/* set to 1 when we may have created
			 * empty parameters.
			 */

  if (!headp) return;

  purge_empty_parameters (headp);

  for (last = &head, p = *headp; p; p = q)
  {
    q = p->next;

    if (!(s = strchr (p->attribute, '*')))
    {

      /*
       * Using RFC 2047 encoding in MIME parameters is explicitly
       * forbidden by that document.  Nevertheless, it's being
       * generated by some software, including certain Lotus Notes to
       * Internet Gateways.  So we actually decode it.
       */

      if (option (OPTRFC2047PARAMS) && p->value && strstr (p->value, "=?"))
	rfc2047_decode (&p->value);
      else if (AssumedCharset)
        convert_nonmime_string (&p->value);

      *last = p;
      last = &p->next;
      p->next = NULL;
    }
    else if (*(s + 1) == '\0')
    {
      *s = '\0';

      s = rfc2231_get_charset (p->value, charset, sizeof (charset));
      rfc2231_decode_one (p->value, s);
      mutt_convert_string (&p->value, charset, Charset, MUTT_ICONV_HOOK_FROM);
      mutt_filter_unprintable (&p->value);

      *last = p;
      last = &p->next;
      p->next = NULL;

      dirty = 1;
    }
    else
    {
      *s = '\0'; s++; /* let s point to the first character of index. */
      for (t = s; *t && isdigit ((unsigned char) *t); t++)
	;
      encoded = (*t == '*');
      *t = '\0';

      /* RFC 2231 says that the index starts at 0 and increments by 1,
         thus an overflow should never occur in a valid message, thus
         the value INT_MAX in case of overflow does not really matter
         (the goal is just to avoid undefined behavior). */
      if (mutt_atoi (s, &index))
        index = INT_MAX;

      conttmp = rfc2231_new_parameter ();
      conttmp->attribute = p->attribute;
      conttmp->value = p->value;
      conttmp->encoded = encoded;
      conttmp->index = index;

      p->attribute = NULL;
      p->value = NULL;
      FREE (&p);

      rfc2231_list_insert (&conthead, conttmp);
    }
  }

  if (conthead)
  {
    rfc2231_join_continuations (last, conthead);
    dirty = 1;
  }

  *headp = head;

  if (dirty)
    purge_empty_parameters (headp);
}

static struct rfc2231_parameter *rfc2231_new_parameter (void)
{
  return safe_calloc (sizeof (struct rfc2231_parameter), 1);
}

static void rfc2231_free_parameter (struct rfc2231_parameter **p)
{
  if (*p)
  {
    FREE (&(*p)->attribute);
    FREE (&(*p)->value);
    FREE (p);		/* __FREE_CHECKED__ */
  }
}

static char *rfc2231_get_charset (char *value, char *charset, size_t chslen)
{
  char *t, *u;

  if (!(t = strchr (value, '\'')))
  {
    charset[0] = '\0';
    return value;
  }

  *t = '\0';
  strfcpy (charset, value, chslen);

  if ((u = strchr (t + 1, '\'')))
    return u + 1;
  else
    return t + 1;
}

static void rfc2231_decode_one (char *dest, char *src)
{
  char *d;

  for (d = dest; *src; src++)
  {
    if (*src == '%' &&
        isxdigit ((unsigned char) *(src + 1)) &&
        isxdigit ((unsigned char) *(src + 2)))
    {
      *d++ = (hexval (*(src + 1)) << 4) | (hexval (*(src + 2)));
      src += 2;
    }
    else
      *d++ = *src;
  }

  *d = '\0';
}

/* insert parameter into an ordered list.
 *
 * Primary sorting key: attribute
 * Secondary sorting key: index
 */

static void rfc2231_list_insert (struct rfc2231_parameter **list,
				 struct rfc2231_parameter *par)
{
  struct rfc2231_parameter **last = list;
  struct rfc2231_parameter *p = *list;
  int c;

  while (p)
  {
    c = strcmp (par->attribute, p->attribute);
    if ((c < 0) || (c == 0 && par->index <= p->index))
      break;

    last = &p->next;
    p = p->next;
  }

  par->next = p;
  *last = par;
}

/* process continuation parameters */

static void rfc2231_join_continuations (PARAMETER **head,
					struct rfc2231_parameter *par)
{
  struct rfc2231_parameter *q;

  char attribute[STRING];
  char charset[STRING];
  char *value = NULL;
  char *valp;
  int encoded;

  size_t l, vl;

  while (par)
  {
    value = NULL; l = 0;

    strfcpy (attribute, par->attribute, sizeof (attribute));

    if ((encoded = par->encoded))
      valp = rfc2231_get_charset (par->value, charset, sizeof (charset));
    else
      valp = par->value;

    do
    {
      if (encoded && par->encoded)
	rfc2231_decode_one (par->value, valp);

      vl = strlen (par->value);

      safe_realloc (&value, l + vl + 1);
      strcpy (value + l, par->value);	/* __STRCPY_CHECKED__ */
      l += vl;

      q = par->next;
      rfc2231_free_parameter (&par);
      if ((par = q))
	valp = par->value;
    } while (par && !strcmp (par->attribute, attribute));

    if (value)
    {
      if (encoded)
	mutt_convert_string (&value, charset, Charset, MUTT_ICONV_HOOK_FROM);
      *head = mutt_new_parameter ();
      (*head)->attribute = safe_strdup (attribute);
      (*head)->value = value;
      head = &(*head)->next;
    }
  }
}

PARAMETER *rfc2231_encode_string (const char *attribute, char *value)
{
  int encode = 0, add_quotes = 0, free_src_value = 0;
  int split = 0, continuation_number = 0;
  size_t dest_value_len = 0, max_value_len = 0, cur_value_len = 0;
  char *cur, *charset = NULL, *src_value = NULL;
  PARAMETER *result = NULL, *current, **lastp;
  BUFFER *cur_attribute, *cur_value;

  cur_attribute = mutt_buffer_pool_get ();
  cur_value = mutt_buffer_pool_get ();

  /*
   * Perform charset conversion
   */
  for (cur = value; *cur; cur++)
    if (*cur < 0x20 || *cur >= 0x7f)
    {
      encode = 1;
      break;
    }

  if (encode)
  {
    if (Charset && SendCharset)
      charset = mutt_choose_charset (Charset, SendCharset,
                                     value, mutt_strlen (value),
                                     &src_value, NULL);
    if (src_value)
      free_src_value = 1;
    if (!charset)
      charset = safe_strdup (Charset ? Charset : "unknown-8bit");
  }
  if (!src_value)
    src_value = value;

  /*
   * Count the size the resultant value will need in total.
   */
  if (encode)
    dest_value_len = mutt_strlen (charset) + 2;  /* charset'' prefix */

  for (cur = src_value; *cur; cur++)
  {
    dest_value_len++;

    if (encode)
    {
      /* These get converted to %xx so need a total of three chars */
      if (*cur < 0x20 || *cur >= 0x7f ||
          strchr (MimeSpecials, *cur) ||
          strchr ("*'%", *cur))
      {
        dest_value_len += 2;
      }
    }
    else
    {
      /* rfc822_cat() will add outer quotes if it finds MimeSpecials. */
      if (!add_quotes && strchr (MimeSpecials, *cur))
        add_quotes = 1;
      /* rfc822_cat() will add a backslash if it finds '\' or '"'. */
      if (*cur == '\\' || *cur == '"')
        dest_value_len++;
    }
  }

  /*
   * Determine if need to split into parameter value continuations
   */
  max_value_len =
    78                      -    /* rfc suggested line length */
    1                       -    /* Leading tab on continuation line */
    mutt_strlen (attribute) -    /* attribute */
    (encode ? 1 : 0)        -    /* '*' encoding marker */
    1                       -    /* '=' */
    (add_quotes ? 2 : 0)    -    /* "...." */
    1;                           /* ';' */

  if (max_value_len < 30)
    max_value_len = 30;

  if (dest_value_len > max_value_len)
  {
    split = 1;
    max_value_len -= 4;          /* '*n' continuation number and extra encoding
                                  * space to keep loop below simpler */
  }

  /*
   * Generate list of parameter continuations.
   */
  lastp = &result;
  cur = src_value;
  if (encode)
  {
    mutt_buffer_printf (cur_value, "%s''", charset);
    cur_value_len = mutt_buffer_len (cur_value);
  }

  while (*cur)
  {
    *lastp = current = mutt_new_parameter ();
    lastp = &current->next;
    mutt_buffer_strcpy (cur_attribute, attribute);
    if (split)
      mutt_buffer_add_printf (cur_attribute, "*%d", continuation_number++);
    if (encode)
      mutt_buffer_addch (cur_attribute, '*');

    while (*cur && (!split || cur_value_len < max_value_len))
    {
      if (encode)
      {
        if (*cur < 0x20 || *cur >= 0x7f ||
            strchr (MimeSpecials, *cur) ||
            strchr ("*'%", *cur))
        {
          mutt_buffer_add_printf (cur_value, "%%%02X", (unsigned char)*cur);
          cur_value_len += 3;
        }
        else
        {
          mutt_buffer_addch (cur_value, *cur);
          cur_value_len++;
        }
      }
      else
      {
        mutt_buffer_addch (cur_value, *cur);
        cur_value_len++;
        if (*cur == '\\' || *cur == '"')
          cur_value_len++;
      }

      cur++;
    }

    current->attribute = safe_strdup (mutt_b2s (cur_attribute));
    current->value = safe_strdup (mutt_b2s (cur_value));

    mutt_buffer_clear (cur_value);
    cur_value_len = 0;
  }

  mutt_buffer_pool_release (&cur_attribute);
  mutt_buffer_pool_release (&cur_value);

  FREE (&charset);
  if (free_src_value)
    FREE (&src_value);

  return result;
}