#ifndef MUCGLY_MOD_H
#define MUCGLY_MOD_H


/* ------------------------------------------------------------
 * Data models:
 * ------------------------------------------------------------ */

/*
 * Mucgly uses Pstate to store the parsing state of the input
 * files. The parser has to know whether we are in or out of macro
 * definition. The "hooks" in the input stream change the state within
 * parsing.
 *
 * Pstate contains Filestack. Filestack is a stack of input files. The
 * input file stack grows when the "lower" level files perform
 * include commands.
 *
 * Filestack contains Stackfiles. Stackfile presents the read status
 * of an input file. It contains character position information for
 * error reporting.
 *
 * Outfile is part of the Pstate output file stack. Each Outfile
 * represents the output file state. Outfile stack is needed to
 * implement redirection of output stream to different files. Output
 * files has to be controlled explicitly from Mucgly files.
 */


/** Default value for hookbeg. */
#define HOOKBEG_DEFAULT "-<"

/** Default value for hookend. */
#define HOOKEND_DEFAULT ">-"

/** Default value for hookesc. */
#define HOOKESC_DEFAULT "\\"

/** Multihook count limit, also array size. */
#define MULTI_LIMIT 128


/**
 * Pair of hooks for macro beginning and end. Plus optional suspension
 * marker.
 */
typedef struct hookpair_s {
  gchar* beg;                   /**< Hookbeg for macro. */
  gchar* end;                   /**< Hookend for macro. */
  gchar* susp;                  /**< Suspender for macro. */
} hookpair_t;


/**
 * Stackfile is an entry in the Filestack. Stackfile is the input file
 * for Mucgly.
 */
typedef struct stackfile_s {

  gchar* filename;     /**< Filename. */
  FILE* fh;            /**< IO stream handle. */
  GString* buf;        /**< Put-back buffer (oldest-is-last). */

  int lineno;          /**< Line number (0->). */
  int column;          /**< Line column (0->). */
  int old_column;      /**< Prev column (used with putback). */

  gboolean macro;      /**< Macro active. */
  int macro_line;      /**< Macro start line. */
  int macro_col;       /**< Macro start column. */
  gboolean eat_tail;   /**< Eat the char after macro (if not EOF). */

  hookpair_t hook;     /**< Pair of hooks for macro boundary. */
  gchar* hookesc;      /**< Hookesc for input file. */
  gchar* eater;        /**< Eater. */

  hookpair_t* multi;   /**< Pairs of hooks for multi-hooking. */
  int     multi_cnt;   /**< Number of pairs in multi-hooking. */

  /** Current hook, as stack to support nesting macros. */
  GList* curhook;

  /** Hookesc is same as hookbeg. Speed-up for input processing. */
  gboolean hook_esc_eq_beg;

  /** Hookesc is same as hookend. Speed-up for input processing. */
  gboolean hook_esc_eq_end;

  /** Lookup-table for the first chars of hooks. Speeds up input processing. */
  guchar hook_1st_chars[ 256 ];

} stackfile_t;



/**
 * Filestack is a stack of files (stackfile_t). Macro processing
 * starts at base file. The base file is allowed to include other
 * files (as input with ":include" command). The included file is
 * added to the top of stack as current file at inclusion. When file
 * EOF is encountered, it is popped of the stack. This makes the
 * character flow continuous from the parser point of view.
 */
typedef struct filestack_s {
  GList* file;           /**< Stack of files. */
} filestack_t;



/**
 * Outfile is output file stream descriptor. Output can be diverted
 * from the default output to another output file temporarely. Note
 * that the diverted output stream has to be closed as well.
 */
typedef struct outfile_s {
  gchar* filename;  /**< Filename. */
  FILE* fh;         /**< Stream handle. */
  int lineno;       /**< Line number (0->). */
  gboolean blocked; /**< Blocked output for IO stream. */
} outfile_t;



/**
 * Parser state for Mucgly.
 */
typedef struct pstate_s {
  filestack_t* fs;    /**< Stack of input streams. */

  GString* check_buf; /**< Preview buffer. */
  GString* macro_buf; /**< Macro content buffer. */
  GString* match_buf; /**< Match str buffer. */

  int in_macro;       /**< Processing within macro. */
  int suspension;     /**< Suspension level. */

  GList* output;      /**< Stack of output streams. */

  gboolean flush;     /**< Flush out-stream immediately. */

  gboolean post_push; /**< Move up in fs after macro processing. */
  gboolean post_pop;  /**< Move down in fs after macro processing. */

  mrb_state* mrb;               /**< MRuby. */

} pstate_t;


/** Hook type enum. */
typedef enum hook_e { hook_none, hook_end, hook_beg, hook_esc } hook_t;


/** Current filestack from pstate_t (for convenience). */
#define ps_has_file(ps)  ((stackfile_t*)(ps)->fs->file)

/** Current stackfile from pstate_t (for convenience). */
#define ps_topfile(ps)   ((stackfile_t*)(ps)->fs->file->data)

/** Current stackfile from filestack_t (for convenience). */
#define fs_topfile(fs) ((stackfile_t*)(fs)->file->data)


extern stackfile_t* stack_default;

/** Pointer to current file. Useful for debugging. */
extern stackfile_t* csf;

/** Ruby side reference to parser. */
extern pstate_t* ruby_ps;


int len_str_cmp( char* str1, char* str2 );
void mucgly_user_info( stackfile_t* sf, char* infotype, char* format, va_list ap );
void mucgly_warn( stackfile_t* sf, char* format, ... );
void mucgly_error( stackfile_t* sf, char* format, ... );
void mucgly_fatal( stackfile_t* sf, char* format, ... );
hookpair_t* hookpair_cpy( hookpair_t* from, hookpair_t* to );
void hookpair_del( hookpair_t* pair );
stackfile_t* sf_new( gchar* filename, stackfile_t* inherit );
void sf_mark_macro( stackfile_t* sf );
void sf_unmark_macro( stackfile_t* sf );
void sf_rem( stackfile_t* sf );
int sf_get( stackfile_t* sf );
gboolean sf_put( stackfile_t* sf, char c );
void sf_set_hook( stackfile_t* sf, hook_t hook, char* value );
void sf_set_eater( stackfile_t* sf, char* value );
void sf_multi_hook( stackfile_t* sf, const char* beg, const char* end, const char* susp );
filestack_t* fs_new( void );
filestack_t* fs_rem( filestack_t* fs );
void fs_push_file( filestack_t* fs, gchar* filename );
void fs_push_file_delayed( filestack_t* fs, gchar* filename );
void fs_pop_file( filestack_t* fs );
int fs_get( filestack_t* fs );
int fs_get_one( filestack_t* fs );
gboolean fs_put( filestack_t* fs, char c );
GString* fs_get_n( filestack_t* fs, int n, GString* ret );
gboolean fs_put_n( filestack_t* fs, gchar* str, int n );
outfile_t* outfile_new( gchar* filename );
void outfile_rem( outfile_t* of );
pstate_t* ps_new( gchar* outfile );
void ps_rem( pstate_t* ps );
gboolean ps_check_hook( pstate_t* ps, int c );
gboolean ps_check( pstate_t* ps, gchar* match, gboolean erase );
gboolean ps_check_hookesc( pstate_t* ps );
void ps_push_curhook( stackfile_t* sf, hookpair_t* pair );
void ps_pop_curhook( stackfile_t* sf );
gboolean ps_check_hookbeg( pstate_t* ps );
gboolean ps_check_hookend( pstate_t* ps );
gboolean ps_check_hooksusp( pstate_t* ps );
gboolean ps_check_eater( pstate_t* ps );
//gchar* ps_current_hookbeg( pstate_t* ps );
//gchar* ps_current_hookend( pstate_t* ps );
//gchar* ps_current_hooksusp( pstate_t* ps );
int ps_in( pstate_t* ps );
void ps_out( pstate_t* ps, int c );
void ps_out_str( pstate_t* ps, gchar* str );
void ps_block_output( pstate_t* ps );
void ps_unblock_output( pstate_t* ps );
void ps_push_file( pstate_t* ps, gchar* filename );
void ps_pop_file( pstate_t* ps );
stackfile_t* ps_current_file( pstate_t* ps );
void ps_start_collect( pstate_t* ps );
void ps_collect( pstate_t* ps, int c );
void ps_collect_str( pstate_t* ps, gchar* str );
void ps_enter_macro( pstate_t* ps );
char* ps_get_macro( pstate_t* ps );
gchar* ps_eval_ruby_str( pstate_t* ps, gchar* str, gboolean to_str, char* ctxt );
void ps_load_ruby_file( pstate_t* ps, gchar* filename );
gboolean ps_eval_cmd( pstate_t* ps );
void ps_process_hook_end_seq( pstate_t* ps, gboolean* do_break );
void ps_process_non_hook_seq( pstate_t* ps, int c, gboolean* do_break );
void ps_process_file( pstate_t* ps, gchar* infile, gchar* outfile );



#endif
