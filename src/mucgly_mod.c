/**
 * @file   mucgly_mod.c
 * @author Tero Isannainen <tero@blackbox.home.network>
 * @date   Sun Oct 30 14:56:21 2016
 * 
 * @brief  Mucgly module.
 * 
 * 
 */


#include <stdlib.h>

#include <glib.h>
#include <glib/gstdio.h>

#include <mruby.h>
#include <mruby/array.h>
#include "mruby/class.h"
#include <mruby/proc.h>
#include <mruby/compile.h>
#include <mruby/string.h>

#include <mucgly_mod.h>


/*
 * If MUCGLY_LIB is defined, mucgly is compiled in Ruby-top (e.g. for
 * gems) mode. Otherwise mucgly is in C-top (main) mode,
 * i.e. standalone binary.
 */
#ifdef MUCGLY_LIB
void mucgly_main( int argc, char** argv );
#endif



/* ------------------------------------------------------------
 * Debugging:
 * ------------------------------------------------------------ */

/**
 * Enable the define if input char based debugging is required. Set
 * GDB breakpoint at mucgly_debug.
 */
void mucgly_debug( void ) {}

#if 0
/** Special char for invoking the debugger. */
# define DEBUG_CHAR '~'
#endif





/* ------------------------------------------------------------
 * Global variables:
 * ------------------------------------------------------------ */

/** Default hook settings. */
stackfile_t* stack_default = NULL;

/** Pointer to current file. Useful for debugging. */
stackfile_t* csf;

/** Ruby side reference to parser. */
pstate_t* ruby_ps;



/* ------------------------------------------------------------
 * Function declarations:
 * ------------------------------------------------------------ */

void sf_update_hook_cache( stackfile_t* sf );
gchar* ps_current_hookbeg( pstate_t* ps );
gchar* ps_current_hookend( pstate_t* ps );
gchar* ps_current_hooksusp( pstate_t* ps );



/* ------------------------------------------------------------
 * Mucgly C-functions:
 * ------------------------------------------------------------ */


/**
 * Compare two strings upto length of str1.
 *
 * @param str1 String 1.
 * @param str2 String 2.
 *
 * @return Match length (0 for no match).
 */
int len_str_cmp( char* str1, char* str2 )
{
  int len = strlen( str1 );

  if ( !strncmp( str1, str2, len ) )
    return len;
  else
    return 0;
}


/**
 * Common routine for user info message output.
 *
 * @param sf        Currently processed file (or NULL).
 * @param infotype  Severity type.
 * @param format    Message (printf) formatter.
 * @param ap        Message args.
 */
void mucgly_user_info_str( stackfile_t* sf, char* infotype, GString* str, char* format, va_list ap )
{
  if ( sf )
    {
      int lineno, column;

      if ( sf->macro )
        {
          lineno = sf->macro_line;
          column = sf->macro_col;
        }
      else
        {
          lineno = sf->lineno;
          column = sf->column;
        }

      g_string_append_printf( str, "mucgly %s in \"%s:%d:%d\": ",
                              infotype,
                              sf->filename,
                              lineno+1,
                              column+1
                              );
    }
  else
    {
      g_string_append_printf( str, "mucgly %s: ", infotype );
    }

  g_string_append_vprintf( str, format, ap );
}


/**
 * Common routine for user info message output.
 *
 * @param sf        Currently processed file (or NULL).
 * @param infotype  Severity type.
 * @param format    Message (printf) formatter.
 * @param ap        Message args.
 */
void mucgly_user_info( stackfile_t* sf, char* infotype, char* format, va_list ap )
{
  GString* out = g_string_sized_new( 0 );
  mucgly_user_info_str( sf, infotype, out, format, ap );
  fputs( out->str, stderr );
  fputc( '\n', stderr );
  fflush( stderr );
  g_string_free( out, TRUE );
}


/**
 * Report warning to user (no exit).
 *
 * @param sf     Current input file.
 * @param format Message formatter.
 */
void mucgly_warn( stackfile_t* sf, char* format, ... )
{
  va_list ap;
  va_start( ap, format );
  mucgly_user_info( sf, "warning", format, ap );
  va_end( ap );
}


/**
 * Report error to user (and exit).
 *
 * @param sf     Current input file.
 * @param format Message formatter.
 */
void mucgly_error( stackfile_t* sf, char* format, ... )
{
  va_list ap;
  va_start( ap, format );
  mucgly_user_info( sf, "error", format, ap );
  va_end( ap );
  exit( EXIT_FAILURE );
}


/**
 * Report fatal error to user (and exit).
 *
 * @param sf     Current input file.
 * @param format Message formatter.
 */
void mucgly_fatal( stackfile_t* sf, char* format, ... )
{
  va_list ap;
  va_start( ap, format );
  mucgly_user_info( sf, "fatal error", format, ap );
  va_end( ap );
  exit( EXIT_FAILURE );
}


/**
 * Raise MRuby exception.
 *
 * @param sf       Current input file.
 * @param infotype Severity type.
 * @param format   Message formatter.
 */
void mucgly_raise( pstate_t* ps, char* infotype, char* format, ... )
{
  GString* out = g_string_sized_new( 0 );
  va_list ap;
  mrb_state* mrb = ps->mrb;
  va_start( ap, format );
  mucgly_user_info_str( ps_topfile(ps), infotype, out, format, ap );
  va_end( ap );
  mrb_raise( mrb, E_RUNTIME_ERROR, (char*) out->str );
  g_string_free( out, TRUE );
}


/**
 * Copy hookpair content.
 *
 * @param from Source.
 * @param to   Destination.
 *
 * @return Destination.
 */
hookpair_t* hookpair_cpy( hookpair_t* from, hookpair_t* to )
{
  to->beg = g_strdup( from->beg );
  to->end = g_strdup( from->end );
  to->susp = g_strdup( from->susp );
  return to;
}


/**
 * Free allocated hookpair memory.
 *
 * @param pair Pair to dealloc.
 */
void hookpair_del( hookpair_t* pair )
{
  g_free( pair->beg );
  g_free( pair->end );
  g_free( pair->susp );
}


/**
 * Create new Stackfile.
 *
 * @param filename Filename (NULL for stdin).
 * @param inherit  Inherit hooks source (NULL for defaults).
 *
 * @return Stackfile.
 */
stackfile_t* sf_new( gchar* filename, stackfile_t* inherit )
{
  stackfile_t* sf;

  sf = g_new0( stackfile_t, 1 );

  if ( filename )
    {
      /* Real file. */

      sf->fh = g_fopen( filename, (gchar*) "r" );
      sf->filename = g_strdup( filename );

      if ( sf->fh == NULL )
        {
          stackfile_t* err_sf;

          /* Get the includer. */
          err_sf = ruby_ps->fs->file->data;

          mucgly_fatal( err_sf, "Can't open \"%s\"", filename );
        }

    }
  else
    {
      /* Use stdin. */
      sf->fh = stdin;
      sf->filename = g_strdup( "<STDIN>" );
    }

  sf->buf = g_string_sized_new( 0 );
  sf->lineno = 0;
  sf->column = 0;
  sf->old_column = 0;

  sf->macro = FALSE;
  sf->macro_line = 0;
  sf->macro_col = 0;
  sf->eat_tail = FALSE;

  sf->multi = NULL;
  sf->multi_cnt = 0;

  sf->curhook = NULL;


  if ( inherit )
    {
      /* Inherited hook values. */
      hookpair_cpy( &inherit->hook, &sf->hook );
      sf->hookesc = g_strdup( inherit->hookesc );

      /* Setup fast-lookup caches. */
      sf_update_hook_cache( sf );

      if ( inherit->multi )
        {
          for ( int i = 0; i < inherit->multi_cnt; i++ )
            {
              sf_multi_hook( sf, inherit->multi[i].beg, inherit->multi[i].end, inherit->multi[i].susp  );
            }
        }
    }
  else
    {
      /* Default hook values. */
      sf->hook.beg = g_strdup( HOOKBEG_DEFAULT );
      sf->hook.end = g_strdup( HOOKEND_DEFAULT );
      sf->hookesc = g_strdup( HOOKESC_DEFAULT );

      /* Setup fast-lookup caches. */
      sf_update_hook_cache( sf );
    }

  return sf;
}


/**
 * Store macro start info.
 *
 * @param sf Stackfile.
 */
void sf_mark_macro( stackfile_t* sf )
{
  sf->macro = TRUE;
  sf->macro_line = sf->lineno;
  sf->macro_col = sf->column;
}


/**
 * Reset macro start info.
 *
 * @param sf Stackfile.
 */
void sf_unmark_macro( stackfile_t* sf )
{
  sf->macro = FALSE;
}


/**
 * Free Stackfile and close the file stream.
 *
 * @param sf Stackfile.
 */
void sf_rem( stackfile_t* sf )
{
  g_string_free( sf->buf, TRUE );

  if ( sf->fh != stdin )
    {
      fclose( sf->fh );
    }

  g_free( sf->filename );

  hookpair_del( &sf->hook );
  g_free( sf->hookesc );

  if ( sf->multi )
    {
      for ( int i = 0; i < sf->multi_cnt; i++ )
        hookpair_del( &sf->multi[i] );
      g_free( sf->multi );
    }

  g_free( sf );
}


/**
 * Get char from Stackfile.
 *
 * @param sf Stackfile.
 *
 * @return Char or EOF.
 */
int sf_get( stackfile_t* sf )
{
  int ret;

 re_read:
  if ( sf->buf->len > 0 )
    {
      /* Pending chars in buffer, don't need to read file (yet). */
      int pos = sf->buf->len-1;

      /* Use buffer as stack with oldest (first to use) on right. */
      ret = sf->buf->str[ pos ];
      g_string_truncate( sf->buf, pos );
    }
  else
    {
      /* Read from actual file stream. */
#ifdef DEBUG_CHAR
      ret = fgetc( sf->fh );
      if ( DEBUG_CHAR == (char) ret )
        {
          mucgly_debug();
          ret = fgetc( sf->fh );
        }
#else
      ret = fgetc( sf->fh );
#endif

    }

  /* Update file point info. */
  if ( ret != EOF )
    {
      if ( ret == '\n' )
        {
          sf->old_column = sf->column;
          sf->lineno++;
          sf->column = 0;
        }
      else
        {
          sf->column++;
        }
    }

  if ( sf->eat_tail == TRUE )
    {
      /* Eat tail if not in EOF. */
      sf->eat_tail = FALSE;
      if ( ret != EOF )
        goto re_read;
    }

  return ret;
}


/**
 * Put char back to Stackfile.
 *
 * @param sf Stackfile.
 * @param c  Char.
 *
 * @return TRUE.
 */
gboolean sf_put( stackfile_t* sf, char c )
{
  if ( c == '\n' )
    {
      sf->lineno--;
      sf->column = sf->old_column;
      sf->old_column = 0;
    }
  else
    {
      sf->column--;
    }

  /* Use buffer as stack with oldest (first to use) on right. */
  g_string_append_c( sf->buf, c );

  return TRUE;
}


/**
 * Update the hooks related cache/lookup entries.
 *
 * @param sf Stackfile.
 */
void sf_update_hook_cache( stackfile_t* sf )
{
  if ( sf->multi )
    {
      /* Esc can never match multihooks. */
      sf->hook_esc_eq_beg = FALSE;
      sf->hook_esc_eq_end = FALSE;

      /* Add the latest addition to 1st char lookup. */
      sf->hook_1st_chars[ sf->multi[ sf->multi_cnt-1 ].beg[0] ] = 1;
      sf->hook_1st_chars[ sf->multi[ sf->multi_cnt-1 ].end[0] ] = 1;
      if ( sf->multi[ sf->multi_cnt-1 ].susp )
        sf->hook_1st_chars[ sf->multi[ sf->multi_cnt-1 ].susp[0] ] = 1;

      sf->hook_1st_chars[ sf->hookesc[0] ] = 1;
    }
  else
    {
      /* Store these equalities for speed-up. */
      if ( !g_strcmp0( sf->hookesc, sf->hook.beg ) )
        sf->hook_esc_eq_beg = TRUE;
      else
        sf->hook_esc_eq_beg = FALSE;

      if ( !g_strcmp0( sf->hookesc, sf->hook.end ) )
        sf->hook_esc_eq_end = TRUE;
      else
        sf->hook_esc_eq_end = FALSE;

      /* Initialize 1st char lookup. */
      memset( sf->hook_1st_chars, 0, 256 * sizeof( guchar ) );

      /* Hook 1st char lookup. */
      sf->hook_1st_chars[ sf->hook.beg[0] ] = 1;
      sf->hook_1st_chars[ sf->hook.end[0] ] = 1;
      sf->hook_1st_chars[ sf->hookesc[0] ] = 1;
    }
}


/**
 * Set hook value.
 *
 * @param sf    Stackfile.
 * @param hook  Hook type.
 * @param value New hook value.
 */
void sf_set_hook( stackfile_t* sf, hook_t hook, char* value )
{
  /* Check that we are not in multi-hook mode. */
  if ( sf->multi && hook != hook_esc )
    {
      /* Disable multi-hook mode. */
      for ( int i = 0; i < sf->multi_cnt; i++ )
        hookpair_del( &sf->multi[i] );
      g_free( sf->multi );
      sf->multi_cnt = 0;
    }

  /* Set values. */
  switch ( hook )
    {
    case hook_beg: g_free( sf->hook.beg ); sf->hook.beg = g_strdup( value ); break;
    case hook_end: g_free( sf->hook.end ); sf->hook.end = g_strdup( value ); break;
    case hook_esc:
      {
        /* Remove old esc from 1st char lookup. */
        sf->hook_1st_chars[ sf->hookesc[0] ] = 0;
        g_free( sf->hookesc );
        sf->hookesc = g_strdup( value );
        break;
      }
    default: break;
    }

  sf_update_hook_cache( sf );
}


/**
 * Set eater value.
 *
 * @param sf     Stackfile.
 * @param value  New eater value.
 */
void sf_set_eater( stackfile_t* sf, char* value )
{
  if ( sf->eater )
    g_free( sf->eater );

  sf->eater = NULL;
  if ( value )
    sf->eater = g_strdup( value );
}



/**
 * Add multi-hook pair.
 *
 * @param sf    Stackfile.
 * @param beg   Hookbeg of pair.
 * @param end   Hookend of pair.
 * @param susp  Suspension.
 */
void sf_multi_hook( stackfile_t* sf, const char* beg, const char* end, const char* susp )
{
  /* Check that hooks don't match escape. */
  if ( !g_strcmp0( sf->hookesc, beg )
       || !g_strcmp0( sf->hookesc, end ) )
    {
      g_print( "mucgly: Esc hook is not allowed to match multihooks\n" );
      exit( EXIT_FAILURE );
    }

  if ( sf->multi == NULL )
    {
      /* Allocate enough pairs. */
      sf->multi = g_new0( hookpair_t, MULTI_LIMIT );
      sf->multi[0] = (hookpair_t) {0,0,0};
      sf->multi_cnt = 0;

      /* Clear non-multi-mode lookups. */
      memset( sf->hook_1st_chars, 0, 256 * sizeof( guchar ) );
    }

  if ( sf->multi_cnt >= (MULTI_LIMIT-1) )
    {
      g_print( "mucgly: Too many multihooks, 127 allowed!\n" );
      exit( EXIT_FAILURE );
    }

  sf->multi[sf->multi_cnt].beg = g_strdup( beg );
  sf->multi[sf->multi_cnt].end = g_strdup( end );
  sf->multi[sf->multi_cnt].susp = g_strdup( susp );

  sf->multi_cnt++;
  sf->multi[sf->multi_cnt] = (hookpair_t) {0,0,0};

  sf_update_hook_cache( sf );
}


/**
 * Create new Filestack.
 *
 * @return Filestack.
 */
filestack_t* fs_new( void )
{
  filestack_t* fs;

  fs = g_new0( filestack_t, 1 );
  fs->file = NULL;
  return fs;
}


/**
 * Free Filestack and close all Stackfiles.
 *
 * @param fs Filestack.
 *
 * @return NULL.
 */
filestack_t* fs_rem( filestack_t* fs )
{
  if ( fs )
    {
      for ( GList* p = fs->file; p; p = p->next )
        {
          sf_rem( (stackfile_t*) p->data );
        }

      g_free( fs );
    }

  return NULL;
}


/**
 * Push file on top of Filestack.
 *
 * @param fs       Filestack.
 * @param filename New top file.
 */
void fs_push_file( filestack_t* fs, gchar* filename )
{
  stackfile_t* sf;

  if ( fs->file )
    /* Additional file. */
    sf = sf_new( filename, fs_topfile(fs) );
  else
    /* First file, i.e. inherit stack_default hooks. */
    sf = sf_new( filename, stack_default );

  /* Push file. */
  fs->file = g_list_prepend( fs->file, sf );

  /* Set global CurrentStackfile handle. */
  csf = (stackfile_t*) fs->file->data;
}


/**
 * Push file on top of Filestack for later use (i.e. current macro is
 * completely processed).
 *
 * @param fs       Filestack.
 * @param filename New top file.
 */
void fs_push_file_delayed( filestack_t* fs, gchar* filename )
{
  fs_push_file( fs, filename );
  fs->file = fs->file->next;
}


/**
 * Pop file from top of Filestack. File is closed.
 *
 * @param fs Filestack.
 */
void fs_pop_file( filestack_t* fs )
{
  stackfile_t* sf;

  sf = fs->file->data;
  sf_rem( sf );
  fs->file = g_list_delete_link( fs->file, fs->file );

  /* Set global CurrentStackfile handle. */
  if ( fs->file )
    csf = (stackfile_t*) fs->file->data;
  else
    csf = NULL;
}


/**
 * Get char from (through) Filestack (i.e. top file). Stop at EOF, and
 * don't pop the file in order to allow putback to the file.
 *
 * @param fs Filestack.
 *
 * @return Char or EOF.
 */
int fs_get( filestack_t* fs )
{
  if ( fs->file != NULL )
    return sf_get( fs_topfile(fs) );
  else
    return EOF;
}


/**
 * Get char from (through) Filestack (i.e. top file). If EOF is
 * encountered, continue with lower file if possible.
 *
 * @param fs Filestack.
 *
 * @return Char or EOF.
 */
int fs_get_one( filestack_t* fs )
{
  int ret;

  if ( fs->file != NULL )
    {
      ret = sf_get( fs_topfile(fs) );

      /* Pop Stackfiles until no files or non-EOF char is received. */
      while ( ret == EOF )
        {
          fs_pop_file( fs );
          if ( fs->file == NULL )
            return EOF;
          ret = sf_get( fs_topfile(fs) );
        }

      return ret;
    }
  else
    {
      return EOF;
    }
}


/**
 * Put back char.
 *
 * @param fs Filestack.
 * @param c  Char.
 *
 * @return TRUE.
 */
gboolean fs_put( filestack_t* fs, char c )
{
  sf_put( fs_topfile(fs), c );
  return TRUE;
}


/**
 * Get n chars from Filestack. If ret is non-null, use that for
 * storage.
 *
 * @param fs  Filestack.
 * @param n   Number of chars to get.
 * @param ret Storage for data.
 *
 * @return Return data.
 */
GString* fs_get_n( filestack_t* fs, int n, GString* ret )
{

  if ( ret == NULL )
    {
      /* New GString is needed for storage. */
      ret = g_string_sized_new( 0 );
    }
  else
    {
      /* Reset size of the existing storage. */
      g_string_set_size( ret, 0 );
    }


  /* Get n chars from current file. If EOF is encountered, reading is
     terminated. */

  for ( int i = 0, c; ; )
    {
      if ( i >= n )
        break;

      c = fs_get( fs );

      if ( c != EOF )
        {
          g_string_append_c( ret, (char) c );
          i++;
        }
      else
        {
          break;
        }
    }

  return ret;
}


/**
 * Put chars in str back to Filestack. If n is 0, then use strlen to
 * get the count.
 *
 * @param fs  Filestack.
 * @param str Content to add.
 * @param n   Number of chars from content.
 *
 * @return True if success.
 */
gboolean fs_put_n( filestack_t* fs, gchar* str, int n )
{
  if ( n == 0 )
    n = strlen( str );

  /* Put chars with newest first (i.e. reverse order), so that oldest
     comes out first. */
  for ( int i = n-1; i >= 0; i-- )
    {
      fs_put( fs, str[ i ] );
    }

  return TRUE;
}


/**
 * Create new Outfile. If filename is NULL, then stream is stdout.
 *
 * @param filename File name (or NULL for stdout).
 *
 * @return Outfile.
 */
outfile_t* outfile_new( gchar* filename )
{
  outfile_t* of;

  of = g_new0( outfile_t, 1 );
  of->lineno = 0;
  of->blocked = FALSE;

  if ( filename )
    {
      /* Disk file output. */

      of->filename = g_strdup( filename );

      of->fh = g_fopen( filename, (gchar*) "w" );

      if ( of->fh == NULL )
        {
          stackfile_t* err_sf;

          /* Get the includer. */
          err_sf = ruby_ps->fs->file->data;

          mucgly_fatal( err_sf, "Can't open \"%s\"", err_sf->filename );
        }
    }
  else
    {
      /* STDOUT output. */
      of->filename = g_strdup( "<STDOUT>" );
      of->fh = stdout;
    }

  return of;
}


/**
 * Free Outfile and close output stream if non-stdout.
 *
 * @param of Outfile.
 */
void outfile_rem( outfile_t* of )
{
  if ( of->fh != stdout )
    fclose( of->fh );

  g_free( of->filename );
  g_free( of );
}


/**
 * Create Pstate. Create input file stack and output file
 * stack. Initialize parsing state.
 *
 * @param outfile Initial input file name.
 *
 * @return Pstate.
 */
pstate_t* ps_new( gchar* outfile )
{
  pstate_t* ps;

  ps = g_new0( pstate_t, 1 );

  ps->fs = fs_new();

  ps->in_macro = 0;
  ps->suspension = 0;

  /* Preview input buffer. */
  ps->check_buf = g_string_sized_new( 0 );

  /* Top level input buffer. */
  ps->macro_buf = g_string_sized_new( 0 );

  /* Match string buffer. */
  ps->match_buf = g_string_sized_new( 0 );

  ps->output = g_list_prepend( ps->output, outfile_new( outfile ) );
  ps->flush = FALSE;

  ps->post_push = FALSE;
  ps->post_pop = FALSE;

  ps->mrb = NULL;

  return ps;
}


/**
 * Free Pstate. Close all input and output files.
 *
 * @param ps Pstate.
 */
void ps_rem( pstate_t* ps )
{
  fs_rem( ps->fs );

  g_string_free( ps->check_buf, TRUE );
  g_string_free( ps->macro_buf, TRUE );

  for ( GList* of = ps->output; of; of = of->next )
    {
      outfile_rem( (outfile_t*) of->data );
    }

  mrb_close( ps->mrb );

  g_free( ps );
}


/**
 * Fast check for current input char being first char of any of the
 * hooks.
 *
 * @param ps Pstate.
 * @param c  Char to check.
 *
 * @return TRUE if next char matches 1st char of any hook.
 */
gboolean ps_check_hook( pstate_t* ps, int c )
{
  stackfile_t* sf;

  if ( c == EOF )
    return FALSE;

  sf = ps_topfile(ps);

  if ( sf->hook_1st_chars[ c ] )
    return TRUE;
  else
    return FALSE;
}


/**
 * Check if input has the match string coming next. Erase the matched
 * string if erase is true and input is matched.
 *
 * @param ps    Pstate.
 * @param match Match string.
 * @param erase Erase matched string (but only if matched).
 *
 * @return True if match.
 */
gboolean ps_check( pstate_t* ps, gchar* match, gboolean erase )
{
  GString* input;
  int len;
  gboolean ret;

  /* Dup match string since it might disappear if Filestack stack is
     popped. */

  g_string_assign( ps->match_buf, match );

  len = strlen( match );
  input = fs_get_n( ps->fs, len, ps->check_buf );

  if ( input->len == 0 )
    {
      /* We are at end-of-file. Let's give up and pop the file from
         filestack. */
      fs_pop_file( ps->fs );
      return FALSE;
    }
  else
    {
      /* At least partial match string was received. */

      ret = !g_strcmp0( input->str, ps->match_buf->str );

      if ( !ret || !erase )
        /* Put back checked chars. */
        fs_put_n( ps->fs, input->str, input->len );

      return ret;
    }
}


/**
 * Check input for hookesc.
 *
 * @param ps Pstate.
 *
 * @return TRUE on match.
 */
gboolean ps_check_hookesc( pstate_t* ps )
{
  if ( !ps_has_file(ps) )
    return FALSE;

  return ps_check( ps, ps_topfile(ps)->hookesc, TRUE );
}


/**
 * Save hookpair on top of stack.
 *
 * @param sf    Stackfile (as context).
 * @param pair  Hookpair to store.
 */
void ps_push_curhook( stackfile_t* sf, hookpair_t* pair )
{
  sf->curhook = g_list_prepend(
    sf->curhook,
    hookpair_cpy( pair, g_new0( hookpair_t, 1 ) ) );
}


/**
 * Pop of top of stack hookpair.
 *
 * @param sf Stackfile (as context).
 */
void ps_pop_curhook( stackfile_t* sf )
{
  hookpair_del( (hookpair_t*) sf->curhook->data );
  sf->curhook = g_list_delete_link( sf->curhook, sf->curhook );
}


/**
 * Check input for hookbeg.
 *
 * @param ps Pstate.
 *
 * @return TRUE on match.
 */
gboolean ps_check_hookbeg( pstate_t* ps )
{
  if ( !ps_has_file(ps) )
    return FALSE;

  gboolean ret;
  stackfile_t* sf = ps_topfile(ps);

  if ( sf->multi )
    {
      ret = FALSE;

      /* Check all hookbegs for match. */
      for ( int i = 0; i < sf->multi_cnt; i++ )
        {
          ret = ps_check( ps, sf->multi[i].beg, TRUE );
          if ( ret )
            {
              ps_push_curhook( sf, &sf->multi[i] );
              break;
            }
        }
    }
  else
    {
      ret = ps_check( ps, sf->hook.beg, TRUE );

      if ( ret )
        ps_push_curhook( sf, &sf->hook );
    }

  return ret;
}


/**
 * Check input for hookend.
 *
 * @param ps Pstate.
 *
 * @return TRUE on match.
 */
gboolean ps_check_hookend( pstate_t* ps )
{
  if ( !ps_has_file(ps) )
    return FALSE;
  else
    return ps_check( ps, ps_current_hookend(ps), TRUE );
}


/**
 * Check input for hooksusp.
 *
 * @param ps Pstate.
 *
 * @return TRUE on match.
 */
gboolean ps_check_hooksusp( pstate_t* ps )
{
  gchar* susp;

  if ( !ps_has_file(ps) )
    return FALSE;
  else if ( susp = ps_current_hooksusp(ps) )
    return ps_check( ps, susp, TRUE );
  else
    return FALSE;
}


/**
 * Check input for eater.
 *
 * @param ps Pstate.
 *
 * @return TRUE on match.
 */
gboolean ps_check_eater( pstate_t* ps )
{
  if ( !ps_has_file(ps) )
    return FALSE;

  if ( ps_topfile(ps)->eater )
    return ps_check( ps, ps_topfile(ps)->eater, TRUE );
  else
    return FALSE;
}


/**
 * Return hookbeg string that opened the current macro.
 *
 * @param ps PState.
 *
 * @return
 */
gchar* ps_current_hookbeg( pstate_t* ps )
{
  hookpair_t* h;
  h = ps_topfile(ps)->curhook->data;
  return h->beg;
}


/**
 * Return hookend string that closes the current macro.
 *
 * @param ps PState.
 *
 * @return
 */
gchar* ps_current_hookend( pstate_t* ps )
{
  hookpair_t* h;
  h = ps_topfile(ps)->curhook->data;
  return h->end;
}


/**
 * Return hooksusp string that suspends the current hookend.
 *
 * @param ps PState.
 *
 * @return
 */
gchar* ps_current_hooksusp( pstate_t* ps )
{
  hookpair_t* h;
  h = ps_topfile(ps)->curhook->data;
  return h->susp;
}


/**
 * Get char (through Filestack).
 *
 * @param ps Pstate.
 *
 * @return Char or EOF.
 */
int ps_in( pstate_t* ps )
{
  return fs_get_one( ps->fs );
}


/**
 * Output char to current output stream.
 *
 * @param ps Pstate.
 * @param c  Char.
 */
void ps_out( pstate_t* ps, int c )
{
  outfile_t* of = ps->output->data;

  if ( of->blocked == FALSE )
    {
      if ( c == '\n' )
        of->lineno++;

      fputc( c, of->fh );
      if ( ps->flush )
        fflush( of->fh );
    }
}


/**
 * Output chars with ps_out.
 *
 * @param ps  Pstate.
 * @param str Output string (or NULL for no output).
 */
void ps_out_str( pstate_t* ps, gchar* str )
{
  if ( str )
    {
      int len = strlen( str );

      for ( int i = 0; i < len; i++ )
        {
          ps_out( ps, str[ i ] );
        }
    }
}


/**
 * Block output stream.
 *
 * @param ps Pstate.
 */
void ps_block_output( pstate_t* ps )
{
  outfile_t* of = ps->output->data;
  of->blocked = TRUE;
}


/**
 * Unblock output stream.
 *
 * @param ps Pstate.
 */
void ps_unblock_output( pstate_t* ps )
{
  outfile_t* of = ps->output->data;
  of->blocked = FALSE;
}


/**
 * Push new output stream on top of output file stack.
 *
 * @param ps       Pstate.
 * @param filename File name.
 */
void ps_push_file( pstate_t* ps, gchar* filename )
{
  ps->output = g_list_prepend( ps->output,
                               outfile_new( filename ) );
}


/**
 * Pop file from output file stack. Close the stream.
 *
 * @param ps Pstate.
 */
void ps_pop_file( pstate_t* ps )
{
  outfile_t* of;

  of = ps->output->data;
  outfile_rem( of );
  ps->output = g_list_delete_link( ps->output, ps->output );
}


/**
 * Return current Stackfile (input stream).
 *
 * @param ps Pstate.
 *
 * @return Stackfile (or NULL if none).
 */
stackfile_t* ps_current_file( pstate_t* ps )
{
  if ( ps == NULL )
    return NULL;
  else if ( !ps_has_file(ps) )
    return NULL;
  else
    return ps_topfile(ps);
}



/**
 * Initialize macro content collection.
 *
 * @param ps Pstate.
 */
void ps_start_collect( pstate_t* ps )
{
  g_string_set_size( ps->macro_buf, 0 );
}


/**
 * Add content to macro.
 *
 * @param ps Pstate.
 * @param c  Char to add.
 */
void ps_collect( pstate_t* ps, int c )
{
  g_string_append_c( ps->macro_buf, c );
}


/**
 * Add str content to macro.
 *
 * @param ps  Pstate.
 * @param str String to add.
 */
void ps_collect_str( pstate_t* ps, gchar* str )
{
  g_string_append( ps->macro_buf, str );
}


/**
 * Enter first level macro and setup state accordingly.
 *
 * @param ps Pstate.
 */
void ps_enter_macro( pstate_t* ps )
{
  /* New macro starting. */
  ps->in_macro++;

  /* Store macro start info. */
  sf_mark_macro( ps_topfile( ps ) );

  ps_start_collect( ps );
}


/**
 * Get current macro content.
 *
 * @param ps Pstate.
 *
 * @return Macro content.
 */
char* ps_get_macro( pstate_t* ps )
{
  if ( ps->macro_buf->str[0] == '+' )
    {
      /* Eat the next char after macro. */
      ps_topfile(ps)->eat_tail = TRUE;
      return &ps->macro_buf->str[1];
    }
  else
    {
      return ps->macro_buf->str;
    }
}


/**
 * Evaluate str as Ruby code and convert result to string. Conversion
 * is performed if execution was successful and conversion was
 * requested.
 *
 * @param ps     Pstate.
 * @param str    Ruby code string.
 * @param to_str Result convert enable.
 * @param ctxt   Context (name) for execution.
 *
 * @return Ruby code execution as string (or NULL).
 */
gchar* ps_eval_ruby_str( pstate_t* ps, gchar* str, gboolean to_str, char* ctxt )
{
  mrb_value ret;

  if ( !ctxt )
    /* Used default context. */
    ctxt = "macro";

  ret = mrb_load_string( ps->mrb, (char*) str );

  if ( ps->mrb->exc ) {
    mrb_value obj;
    gchar* str;

    /* Error handling. */
    obj = mrb_funcall( ps->mrb, mrb_obj_value( ps->mrb->exc ), "inspect", 0 );
    str = RSTRING_PTR(obj);
    mucgly_error( ps_current_file( ps ), str );

    return NULL;
  }

  /* Successfull execution. */
  if ( to_str )
    {
      if ( mrb_obj_is_kind_of( ps->mrb, ret, ps->mrb->string_class ) )
        {
          return ( (gchar*) RSTRING_PTR( ret ) );
        }
      else
        {
          /* Convert to Ruby String. */
          ret = mrb_inspect( ps->mrb, ret );
          return ( (gchar*) RSTRING_PTR( ret ) );
        }
    }
  else
    {
      return NULL;
    }
}


/**
 * Load file with Ruby intepreter.
 *
 * @param ps       Pstate.
 * @param filename Source file.
 */
void ps_load_ruby_file( pstate_t* ps, gchar* filename )
{
  FILE* ufh = fopen( filename, "r" );

  if ( ufh )
    {
      mrb_load_file( ps->mrb, ufh );
      fclose( ufh );
    }
}


/**
 * Execute Mucgly command/macro.
 *
 * @param ps Pstate.
 *
 * @return TRUE if input processing should be aborted.
 */
gboolean ps_eval_cmd( pstate_t* ps )
{
  char* cmd;

  cmd = ps_get_macro( ps );

  if ( cmd[0] == ':' )
    {

      /* Mucgly internal command. */

      int len;

      if ( 0 ) {}
      else if ( ( len = len_str_cmp( ":hookbeg", cmd ) ) )
        sf_set_hook( ps_topfile(ps), hook_beg, &cmd[ len+1 ] );
      else if ( ( len = len_str_cmp( ":hookend", cmd ) ) )
        sf_set_hook( ps_topfile(ps), hook_end, &cmd[ len+1 ] );
      else if ( ( len = len_str_cmp( ":hookesc", cmd ) ) )
        sf_set_hook( ps_topfile(ps), hook_esc, &cmd[ len+1 ] );
      else if ( ( len = len_str_cmp( ":eater", cmd ) ) )
        sf_set_eater( ps_topfile(ps), &cmd[ len+1 ] );

      else if ( ( len = len_str_cmp( ":hookall", cmd ) ) )
        {
          sf_set_hook( ps_topfile(ps), hook_beg, &cmd[ len+1 ] );
          sf_set_hook( ps_topfile(ps), hook_end, &cmd[ len+1 ] );
          sf_set_hook( ps_topfile(ps), hook_esc, &cmd[ len+1 ] );
        }

      else if ( ( len = len_str_cmp( ":hook", cmd ) ) )
        {
          gchar** pieces;

          pieces = g_strsplit( &cmd[ len+1 ], " ", 2 );
          if ( g_strv_length( pieces ) == 2 )
            {
              /* Two hooks separated by space. */
              sf_set_hook( ps_topfile(ps), hook_beg, pieces[0] );
              sf_set_hook( ps_topfile(ps), hook_end, pieces[1] );
            }
          else
            {
              /* Only one hook specified. */
              sf_set_hook( ps_topfile(ps), hook_beg, pieces[0] );
              sf_set_hook( ps_topfile(ps), hook_end, pieces[0] );
            }
          g_strfreev( pieces );
        }

      else if ( ( len = len_str_cmp( ":include", cmd ) ) )
        {
          fs_push_file_delayed( ps->fs, &cmd[ len+1 ] );
          ps->post_push = TRUE;
        }

      else if ( ( len = len_str_cmp( ":source", cmd ) ) )
        ps_load_ruby_file( ps, &cmd[ len+1 ] );

      else if ( ( len = len_str_cmp( ":block", cmd ) ) )
        ps_block_output( ps );

      else if ( ( len = len_str_cmp( ":unblock", cmd ) ) )
        ps_unblock_output( ps );

      else if ( ( len = len_str_cmp( ":comment", cmd ) ) )
        {
          /* Do nothing. */
        }

      else if ( ( len = len_str_cmp( ":exit", cmd ) ) )
        {
          /* Exit processing. */
          return TRUE;
        }

      else
        {
          mucgly_error( ps_topfile(ps), "Unknown internal command: \"%s\"", &cmd[len+1] );
        }
    }

  else if ( cmd[0] == '.' )
    {
      /* Mucgly variable output. */
      ps_out_str( ps, ps_eval_ruby_str( ps, &cmd[1], TRUE, NULL ) );
    }

  else if ( cmd[0] == '/' )
    {
      /* Mucgly comment, do nothing. */
    }

  else if ( cmd[0] == '#' )
    {
      /* Postpone evaluation (to next round). */

      ps_out_str( ps, ps_current_hookbeg( ps ) );

      /* Remove one # sign. */
      ps_out_str( ps, &cmd[1] );

      ps_out_str( ps, ps_current_hookend( ps ) );
    }

  else
    {
      /* Ruby code execution. */
      ps_eval_ruby_str( ps, cmd, FALSE, NULL );
    }

  return FALSE;
}


/**
 * Processing routine for hookend phase.
 *
 * @param ps       Pstate.
 * @param do_break True if current processing phase is over.
 */
void ps_process_hook_end_seq( pstate_t* ps, gboolean* do_break )
{
  ps->in_macro--;

  if ( ps->in_macro < 0 )
    {
      mucgly_fatal( ps_topfile(ps), "Internal error in macro status..." );
    }
  else if ( ps->in_macro )
    {
      /* Multilevel macro decrement. */
      /* Output the consumed hookend. */
      ps_out_str( ps, ps_current_hookend( ps ) );
      ps_pop_curhook( ps_topfile(ps) );
      *do_break = FALSE;
    }
  else
    {
      /* Back to base level from macro, eval the macro. */
      *do_break = ps_eval_cmd( ps );
      sf_unmark_macro( ps_topfile( ps ) );
      ps_pop_curhook( ps_topfile(ps) );

      if ( ps->post_push == TRUE )
        {
          ps->post_push = FALSE;
          ps->fs->file = ps->fs->file->prev;
        }

      if ( ps->post_pop )
        {
          ps->post_pop = FALSE;
          fs_pop_file( ps->fs );
        }
    }
}


/**
 * Processing routine for non-hook chars.
 *
 * @param ps        Pstate.
 * @param c         Current char input.
 * @param do_break  True if should break from current phase.
 */
void ps_process_non_hook_seq( pstate_t* ps, int c, gboolean* do_break )
{
  /* Non-hook input. */
  if ( ps->in_macro )
    {
      *do_break = FALSE;

      if ( c == EOF )
        mucgly_error( ps_current_file(ps), "Got EOF within macro!" );

      /* Add content to macro. */
      ps_collect( ps, c );
    }
  else
    {
      if ( c == EOF )
        {
          *do_break = TRUE;
        }
      else
        {
          *do_break = FALSE;

          /* Output to current output file. */
          ps_out( ps, c );
        }
    }
}


/**
 * Process input file (or stack of files).
 *
 * @param ps      Pstate.
 * @param infile  Input file name.
 * @param outfile Output file name (or NULL). Outfile string is freed.
 */
void ps_process_file( pstate_t* ps, gchar* infile, gchar* outfile )
{
  int c;
  gboolean do_break = FALSE;

  fs_push_file( ps->fs, infile );

  if ( outfile )
    {
      ps_push_file( ps, outfile );
      g_free( outfile );
    }

  /* ------------------------------------------------------------
   * Process input:
   * ------------------------------------------------------------ */

  for (;;)
    {

      /* For each input char, we must explicitly read it since
         otherwise the Filestack does not operate correctly, i.e. we
         are not allowed to put back to stream after EOF has been
         encountered. Files are popped automatically after EOF. */

      c = ps_in( ps );

      /* Check if next char starts one of the hooks. */
      if ( ps_check_hook( ps, c ) )
        {

          fs_put( ps->fs, c );

          /* Escape is always checked before other hooks. */
          if ( ps_check_hookesc( ps ) )
            {
              /* Handle ESCAPEs depending whether in macro or not. */

              if ( ps->in_macro )
                {

                  /* Escape in macro. */

                  /* Just transfer the following char to output. */
                  c = ps_in( ps );

                  /* Error, if EOF in macro. */
                  if ( c == EOF )
                    {
                      mucgly_error( ps_current_file(ps), "Got EOF within macro!" );
                    }
                  else
                    {
                      if ( ( c == ' ' || c == '\n' ) &&
                           ps_topfile(ps)->hook_esc_eq_end )
                        {
                          /* Space/newline and hookesc is same as hookbeg. */
                          ps_process_hook_end_seq( ps, &do_break );
                          if ( do_break )
                            break;
                        }
                      else if ( ps_topfile(ps)->eater
                                && ps_topfile(ps)->eater[0] == c )
                        {
                          fs_put( ps->fs, c );
                          if ( ps_check_eater( ps ) )
                            ps_in( ps );
                          else
                            ps_collect( ps, c );
                        }
                      else
                        {
                          ps_collect( ps, c );
                        }
                    }
                }
              else
                {

                  /* Escape outside macro. */

                  /* Map next char according to escape rules. */
                  c = ps_in( ps );

                  if ( c == EOF )
                    {
                      /* EOF */
                      break;
                    }
                  else if ( ps_topfile(ps)->eater
                            && ps_topfile(ps)->eater[0] == c )
                    {
                      fs_put( ps->fs, c );
                      if ( ps_check_eater( ps ) )
                        ps_in( ps );
                      else
                        ps_out( ps, c );
                    }
                  else
                    {

                      switch ( (char)c )
                        {

                        case '\n':
                          /* Eat newlines. */
                          break;

                        case ' ':
                          /* Eat spaces. */
                          break;

                        default:

                          if ( ps_topfile(ps)->hook_esc_eq_beg )
                            {

                              if ( ps_topfile(ps)->hookesc[1] == 0
                                   && c == ps_topfile(ps)->hookesc[0] )
                                {
                                  /* Escape is one char long and following
                                     char was ESC (i.e. escaped escape). */
                                  ps_out( ps, c );
                                }
                              else
                                {
                                  /* Escape is same as hookbeg and escape is
                                     not used to eat out spaces. */

                                  /* Put back the extra char (for safety/clarity). */
                                  fs_put( ps->fs, c );

                                  /* Push hook here. For non-esc hooks
                                     this is done while matching for
                                     hook. */
                                  ps_push_curhook( ps_topfile(ps), &ps_topfile(ps)->hook );

                                  /* Start to collect the macro content. */
                                  ps_enter_macro( ps );
                                }
                            }
                          else
                            {
                              /* Literal output. */
                              ps_out( ps, c );
                            }
                        }
                    }

                }
            }
          else if ( ps->in_macro && ps_check_hooksusp( ps ) )
            {
              ps->suspension++;
              ps_collect_str( ps, ps_current_hooksusp( ps ) );
            }
          else if ( ps->in_macro && ps_check_hookend( ps ) )
            {
              /* Hookend has priority over hookbeg if we are in
                 macro. Also hookend is ignored when outside macro. */
              if ( ps->suspension == 0 )
                {
                  ps_process_hook_end_seq( ps, &do_break );
                  if ( do_break )
                    break;
                }
              else
                {
                  ps->suspension--;
                  ps_collect_str( ps, ps_current_hookend( ps ) );
                }
            }
          else if ( ps_check_hookbeg( ps ) )
            {

              if ( ps->in_macro )
                {
                  /* Macro in macro. */

                  /* Increase macro level. */
                  ps->in_macro++;

                  /* Output the consumed hookbeg. */
                  ps_out_str( ps, ps_current_hookbeg( ps ) );
                }
              else
                {
                  ps_enter_macro( ps );
                }
            }
          else
            {
              c = ps_in( ps );
              ps_process_non_hook_seq( ps, c, &do_break );
              if ( do_break )
                break;
            }
        }
      else
        {
          ps_process_non_hook_seq( ps, c, &do_break );
          if ( do_break )
            break;
        }
    }

  if ( outfile )
    ps_pop_file( ps );

}



/* ------------------------------------------------------------
 * Mucgly MRuby-functions:
 * ------------------------------------------------------------ */


/**
 * Mucgly.write method. Write output current output without NL.
 *
 * @param obj  Not used.
 * @param rstr Written string (Ruby String).
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_write( mrb_state* mrb, mrb_value self )
{
  mrb_value obj;
  char* str;

  mrb_get_args( mrb, "o", &obj );
  if ( !mrb_obj_is_kind_of( mrb, obj, mrb->string_class ) )
      obj = mrb_inspect( mrb, obj );
  str = RSTRING_PTR(obj);

  ps_out_str( ruby_ps, str );

  return mrb_nil_value();
}


/**
 * Mucgly.puts method. Write output current output with NL.
 *
 * @param obj  Not used.
 * @param rstr Written string (Ruby String).
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_puts( mrb_state* mrb, mrb_value self )
{
  mrb_value obj;
  char* str;

  mrb_get_args( mrb, "o", &obj );
  if ( !mrb_obj_is_kind_of( mrb, obj, mrb->string_class ) )
      obj = mrb_inspect( mrb, obj );
  str = RSTRING_PTR(obj);

  ps_out_str( ruby_ps, str );
  ps_out( ruby_ps, '\n' );

  return mrb_nil_value();
}


/**
 * Mucgly.hookbeg method. Get hookbeg.
 *
 * @param obj  Not used.
 *
 * @return Hookbeg as Ruby String.
 */
static mrb_value
mrb_mucgly_hookbeg( mrb_state* mrb, mrb_value self )
{
  return mrb_str_new_cstr( mrb, ps_current_file( ruby_ps )->hook.beg );
}


/**
 * Mucgly.hookend method. Get hookend.
 *
 * @param obj  Not used.
 *
 * @return Hookend as Ruby String.
 */
static mrb_value
mrb_mucgly_hookend( mrb_state* mrb, mrb_value self )
{
  return mrb_str_new_cstr( mrb, ps_current_file( ruby_ps )->hook.end );
}


/**
 * Mucgly.hookesc method. Get hookesc.
 *
 * @param obj  Not used.
 *
 * @return Hookesc as Ruby String.
 */
static mrb_value
mrb_mucgly_hookesc( mrb_state* mrb, mrb_value self )
{
  return mrb_str_new_cstr( mrb, ps_current_file( ruby_ps )->hookesc );
}


/**
 * Mucgly.sethook method. Set both hookben and hookend.
 *
 * @param obj  Not used.
 * @param ary  Array of hookpairs.
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_sethook( mrb_state* mrb, mrb_value self )
{
  char* beg, *end;

  mrb_get_args( mrb, "zz", &beg, &end );

  sf_set_hook( ps_topfile( ruby_ps ), hook_beg, beg );
  sf_set_hook( ps_topfile( ruby_ps ), hook_end, end );

  return mrb_nil_value();
}


/**
 * Mucgly.sethookbeg method. Set hookbeg.
 *
 * @param obj  Not used.
 * @param rstr Hookbeg string (Ruby String).
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_sethookbeg( mrb_state* mrb, mrb_value self )
{
  char* str;
  mrb_get_args( mrb, "z", &str );
  sf_set_hook( ps_topfile( ruby_ps ), hook_beg, str );
  return mrb_nil_value();
}


/**
 * Mucgly.sethookend method. Set hookend.
 *
 * @param obj  Not used.
 * @param rstr Hookend string (Ruby String).
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_sethookend( mrb_state* mrb, mrb_value self )
{
  char* str;
  mrb_get_args( mrb, "z", &str );
  sf_set_hook( ps_topfile( ruby_ps ), hook_end, str );
  return mrb_nil_value();
}


/**
 * Mucgly.sethookesc method. Set hookesc.
 *
 * @param obj  Not used.
 * @param rstr Hookesc string (Ruby String).
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_sethookesc( mrb_state* mrb, mrb_value self )
{
  char* str;
  mrb_get_args( mrb, "z", &str );
  sf_set_hook( ps_topfile( ruby_ps ), hook_esc, str );
  return mrb_nil_value();
}


/**
 * Mucgly.seteater method. Set eater.
 *
 * @param obj  Not used.
 * @param rstr Eater string (Ruby String).
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_seteater( mrb_state* mrb, mrb_value self )
{
  mrb_value tmp;
  char* str;

  mrb_get_args( mrb, "o", &tmp );

  if ( mrb_obj_is_kind_of( mrb, tmp, mrb->nil_class ) )
    {
      sf_set_eater( ps_topfile( ruby_ps ), NULL );
    }
  else if ( mrb_obj_is_kind_of( mrb, tmp, mrb->string_class ) )
    {
      str = RSTRING_PTR( tmp );
      sf_set_eater( ps_topfile( ruby_ps ), str );
    }
  else
    {
      mucgly_raise( ruby_ps, "error", "Eater must be a string or nil!" );
   }

  return mrb_nil_value();
}


/**
 * Mucgly.multihook method. Add multihook pairs.
 *
 * Arg options:
 *  [ hb1, he1, su1 ], [ hb2, he2, su2 ], [ hb3, he3 ], ...
 *  [ hb1, he1, hb2, he2, hb3, he3 ]
 *  hb1, he1, hb2, he2, hb3, he3, ...
 *
 * @param obj  Not used.
 * @param ary  Array of hookpairs.
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_multihook( mrb_state* mrb, mrb_value self )
{
  char* beg, *end, *susp;

  mrb_value* argv;
  int argc;
  
  mrb_get_args( mrb, "*", &argv, &argc );
  
  if ( mrb_obj_is_kind_of( mrb, argv[0], mrb->string_class ) )
    {
      /* Even (hb&be pairs) number of strings. */

      if ( ( argc % 2 ) == 0 )
        {
          for ( int i = 0; i < argc; i += 2 )
            {
              beg = RSTRING_PTR( argv[i] );
              end = RSTRING_PTR( argv[i+1] );
              sf_multi_hook( ps_topfile( ruby_ps ), beg, end, NULL );
            }
        }
      else
        {
          mucgly_raise( ruby_ps, "error", "hookbeg/hookend pairs expected for multihook!" );
        }
    }
  else if ( argc == 1
            && mrb_obj_is_kind_of( mrb, argv[0], mrb->array_class )
            && ( RARRAY_LEN( argv[0] ) % 2 == 0 ) )
    {
      
      /* Even number of strings within an array. */

      for ( int i = 0; i < RARRAY_LEN( argv[0] ); i += 2 )
        {
          beg = RSTRING_PTR( mrb_ary_ref( mrb, argv[0], i ) );
          end = RSTRING_PTR( mrb_ary_ref( mrb, argv[0], i+1 ) );
          sf_multi_hook( ps_topfile( ruby_ps ), beg, end, NULL );
        }

    }
  else
    {

      /* Number of arrays with 2/3 entries each. */

      for ( int i = 0; i < argc; i++ )
        {
          if ( mrb_ary_len( mrb, argv[i] ) == 2 )
            {
              beg = RSTRING_PTR( mrb_ary_ref( mrb, argv[i], 0 ) );
              end = RSTRING_PTR( mrb_ary_ref( mrb, argv[i], 1 ) );
              sf_multi_hook( ps_topfile( ruby_ps ), beg, end, NULL );
            }
          else if ( mrb_ary_len( mrb, argv[i] ) == 3 )
            {
              beg = RSTRING_PTR( mrb_ary_ref( mrb, argv[i], 0 ) );
              end = RSTRING_PTR( mrb_ary_ref( mrb, argv[i], 1 ) );
              susp = RSTRING_PTR( mrb_ary_ref( mrb, argv[i], 2 ) );
              sf_multi_hook( ps_topfile( ruby_ps ), beg, end, susp );
            }
          else
            {
              mucgly_raise( ruby_ps, "error", "Array argument must hold either hookbeg/hookend pairs or triplets including suspension!" );
            }
        }

    }

  return mrb_nil_value();
}


/**
 * Mucgly.ifilename method. Gets current input file name.
 *
 * @param obj  Not used.
 *
 * @return Input file name (Ruby String).
 */
static mrb_value
mrb_mucgly_ifilename( mrb_state* mrb, mrb_value self )
{
  return mrb_str_new_cstr( mrb, ps_topfile(ruby_ps)->filename );
}


/**
 * Mucgly.ilinenumber method. Gets current input file line.
 *
 * @param obj  Not used.
 *
 * @return Input file line number (Ruby Fixnum).
 */
static mrb_value
mrb_mucgly_ilinenumber( mrb_state* mrb, mrb_value self )
{
  return mrb_fixnum_value( ps_topfile(ruby_ps)->lineno+1 );
}


/**
 * Mucgly.ofilename method. Gets current output file name.
 *
 * @param obj  Not used.
 *
 * @return Input file name (Ruby String).
 */
static mrb_value
mrb_mucgly_ofilename( mrb_state* mrb, mrb_value self )
{
  outfile_t* of;
  of = ruby_ps->output->data;
  return mrb_str_new_cstr( mrb, of->filename );
}


/**
 * Mucgly.olinenumber method. Gets current output file line.
 *
 * @param obj  Not used.
 *
 * @return Output file line number (Ruby Fixnum).
 */
static mrb_value
mrb_mucgly_olinenumber( mrb_state* mrb, mrb_value self )
{
  outfile_t* of;
  of = ruby_ps->output->data;
  return mrb_fixnum_value( (of->lineno+1) );
}


/**
 * Mucgly.pushinput method. Push new input stream.
 *
 * @param obj  Not used.
 * @param rstr Input file name (Ruby String).
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_pushinput( mrb_state* mrb, mrb_value self )
{
  char* str;

  mrb_get_args( mrb, "z", &str );

  fs_push_file_delayed( ruby_ps->fs, str );
  ruby_ps->post_push = TRUE;

  return mrb_nil_value();
}


/**
 * Mucgly.closeinput method. Pop input stream and close file stream.
 *
 * @param obj  Not used.
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_closeinput( mrb_state* mrb, mrb_value self )
{
  ruby_ps->post_pop = TRUE;
  return mrb_nil_value();
}


/**
 * Mucgly.pushoutput method. Push new output stream.
 *
 * @param obj  Not used.
 * @param rstr Output file name (Ruby String).
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_pushoutput( mrb_state* mrb, mrb_value self )
{
  char* str;

  mrb_get_args( mrb, "z", &str );
  ps_push_file( ruby_ps, str );

  return mrb_nil_value();
}


/**
 * Mucgly.closeoutput method. Pop output stream and close file stream.
 *
 * @param obj  Not used.
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_closeoutput( mrb_state* mrb, mrb_value self )
{
  ps_pop_file( ruby_ps );
  return mrb_nil_value();
}


/**
 * Mucgly.block method. Block output.
 *
 * @param obj  Not used.
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_block( mrb_state* mrb, mrb_value self )
{
  ps_block_output( ruby_ps );
  return mrb_nil_value();
}


/**
 * Mucgly.block method. Block output.
 *
 * @param obj  Not used.
 *
 * @return nil.
 */
static mrb_value
mrb_mucgly_unblock( mrb_state* mrb, mrb_value self )
{
  ps_unblock_output( ruby_ps );
  return mrb_nil_value();
}



#define mrb_func_reg_none(klass,name) mrb_define_module_function( mrb, mrb_ ## klass, # name, mrb_  ## klass ## _ ## name, MRB_ARGS_NONE() );
#define mrb_func_reg_req(klass,name,args) mrb_define_module_function( mrb, mrb_ ## klass, # name, mrb_ ## klass ## _ ## name, MRB_ARGS_REQ(args) );
#define mrb_func_reg_any(klass,name) mrb_define_module_function( mrb, mrb_ ## klass, # name, mrb_  ## klass ## _ ## name, MRB_ARGS_ANY() );


void
mrb_mruby_mucgly_gem_init( mrb_state* mrb )
{
  struct RClass *mrb_mucgly;

  mrb_mucgly = mrb_define_module( mrb, "Mucgly" );

  mrb_func_reg_req(  mucgly, write, 1 );
  mrb_func_reg_req(  mucgly, puts, 1 );
  mrb_func_reg_none( mucgly, hookbeg );
  mrb_func_reg_none( mucgly, hookend );
  mrb_func_reg_none( mucgly, hookesc );
  mrb_func_reg_any(  mucgly, sethook );
  mrb_func_reg_req(  mucgly, sethookbeg, 1 );
  mrb_func_reg_req(  mucgly, sethookend, 1 );
  mrb_func_reg_req(  mucgly, sethookesc, 1 );
  mrb_func_reg_req(  mucgly, seteater, 1 );
  mrb_func_reg_req(  mucgly, multihook, 3 );

  mrb_func_reg_none( mucgly, ifilename );
  mrb_func_reg_none( mucgly, ilinenumber );
  mrb_func_reg_none( mucgly, ofilename );
  mrb_func_reg_none( mucgly, olinenumber );

  mrb_func_reg_req(  mucgly, pushinput, 1 );
  mrb_func_reg_none( mucgly, closeinput );
  mrb_func_reg_req(  mucgly, pushoutput, 1 );
  mrb_func_reg_none( mucgly, closeoutput );

  mrb_func_reg_none( mucgly, block );
  mrb_func_reg_none( mucgly, unblock );
}


void
mrb_mruby_mucgly_gem_final( mrb_state* mrb )
{
}
