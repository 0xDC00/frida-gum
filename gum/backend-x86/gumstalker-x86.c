/*
 * Copyright (C) 2009-2021 Ole André Vadla Ravnås <oleavr@nowsecure.com>
 * Copyright (C) 2010-2013 Karl Trygve Kalleberg <karltk@boblycat.org>
 * Copyright (C) 2020      Duy Phan Thanh <phanthanhduypr@gmail.com>
 *
 * Licence: wxWindows Library Licence, Version 3.1
 */

#include "gumstalker.h"

#include "gummetalhash.h"
#include "gumx86reader.h"
#include "gumx86writer.h"
#include "gummemory.h"
#include "gumx86relocator.h"
#include "gumspinlock.h"
#include "gumtls.h"
#ifdef HAVE_WINDOWS
# include "gumexceptor.h"
#endif

#include <stdlib.h>
#include <string.h>
#ifdef HAVE_WINDOWS
# define VC_EXTRALEAN
# include <windows.h>
# include <psapi.h>
# include <tchar.h>
#endif

#define GUM_CODE_SLAB_SIZE_INITIAL  (128 * 1024)
#define GUM_CODE_SLAB_SIZE_DYNAMIC  (4 * 1024 * 1024)
#define GUM_DATA_SLAB_SIZE_INITIAL  (GUM_CODE_SLAB_SIZE_INITIAL / 5)
#define GUM_DATA_SLAB_SIZE_DYNAMIC  (GUM_CODE_SLAB_SIZE_DYNAMIC / 5)
#define GUM_SCRATCH_SLAB_SIZE       16384
#define GUM_EXEC_BLOCK_MIN_CAPACITY 1024

#if GLIB_SIZEOF_VOID_P == 4
# define GUM_INVALIDATE_TRAMPOLINE_SIZE            16
# define GUM_STATE_PRESERVE_TOPMOST_REGISTER_INDEX 3
# define GUM_IC_MAGIC_EMPTY                        0xdeadface
# define GUM_IC_MAGIC_SCRATCH                      0xcafef00d
#else
# define GUM_INVALIDATE_TRAMPOLINE_SIZE            17
# define GUM_STATE_PRESERVE_TOPMOST_REGISTER_INDEX 9
# define GUM_IC_MAGIC_EMPTY                        0xbaadd00ddeadface
# define GUM_IC_MAGIC_SCRATCH                      0xbaadd00dcafef00d
#endif
#define GUM_MINIMAL_PROLOG_RETURN_OFFSET \
    ((GUM_STATE_PRESERVE_TOPMOST_REGISTER_INDEX + 2) * sizeof (gpointer))
#define GUM_FULL_PROLOG_RETURN_OFFSET \
    (sizeof (GumCpuContext) + sizeof (gpointer))
#define GUM_THUNK_ARGLIST_STACK_RESERVE 64 /* x64 ABI compatibility */

#define GUM_STALKER_LOCK(o) g_mutex_lock (&(o)->mutex)
#define GUM_STALKER_UNLOCK(o) g_mutex_unlock (&(o)->mutex)

typedef struct _GumInfectContext GumInfectContext;
typedef struct _GumDisinfectContext GumDisinfectContext;
typedef struct _GumActivation GumActivation;
typedef struct _GumInvalidateContext GumInvalidateContext;
typedef struct _GumCallProbe GumCallProbe;

typedef struct _GumExecCtx GumExecCtx;
typedef guint GumExecCtxMode;
typedef void (* GumExecHelperWriteFunc) (GumExecCtx * ctx, GumX86Writer * cw);
typedef gpointer (GUM_THUNK * GumExecCtxReplaceCurrentBlockFunc) (
    GumExecCtx * ctx, gpointer start_address);

typedef struct _GumExecBlock GumExecBlock;
typedef guint GumExecBlockFlags;

typedef struct _GumExecFrame GumExecFrame;

typedef struct _GumCodeSlab GumCodeSlab;
typedef struct _GumDataSlab GumDataSlab;
typedef struct _GumSlab GumSlab;

typedef guint GumPrologType;
typedef guint GumCodeContext;
typedef struct _GumGeneratorContext GumGeneratorContext;
typedef struct _GumCalloutEntry GumCalloutEntry;
typedef struct _GumInstruction GumInstruction;
typedef struct _GumBranchTarget GumBranchTarget;
typedef guint GumBackpatchType;
typedef struct _GumBackpatchCall GumBackpatchCall;
typedef struct _GumBackpatchRet GumBackpatchRet;
typedef struct _GumBackpatchJmp GumBackpatchJmp;
typedef struct _GumBackpatchInlineCache GumBackpatchInlineCache;
typedef struct _GumIcEntry GumIcEntry;

typedef guint GumVirtualizationRequirements;

#ifdef HAVE_WINDOWS
# if GLIB_SIZEOF_VOID_P == 8
typedef DWORD64 GumNativeRegisterValue;
# else
typedef DWORD GumNativeRegisterValue;
# endif
#endif

enum
{
  PROP_0,
  PROP_IC_ENTRIES,
};

struct _GumStalker
{
  GObject parent;

  guint ic_entries;

  gsize ctx_size;
  gsize ctx_header_size;

  goffset frames_offset;
  gsize frames_size;

  goffset thunks_offset;
  gsize thunks_size;

  goffset code_slab_offset;
  gsize code_slab_size_initial;
  gsize code_slab_size_dynamic;

  goffset data_slab_offset;
  gsize data_slab_size_initial;
  gsize data_slab_size_dynamic;

  goffset scratch_slab_offset;
  gsize scratch_slab_size;

  gsize page_size;
  GumCpuFeatures cpu_features;
  gboolean is_rwx_supported;

  GMutex mutex;
  GSList * contexts;
  GumTlsKey exec_ctx;

  GArray * exclusions;
  gint trust_threshold;
  volatile gboolean any_probes_attached;
  volatile gint last_probe_id;
  GumSpinlock probe_lock;
  GHashTable * probe_target_by_id;
  GHashTable * probe_array_by_address;

#ifdef HAVE_WINDOWS
  GumExceptor * exceptor;
# if GLIB_SIZEOF_VOID_P == 4
  gpointer user32_start, user32_end;
  gpointer ki_user_callback_dispatcher_impl;
  GArray * wow_transition_impls;
# endif
#endif
};

struct _GumInfectContext
{
  GumStalker * stalker;
  GumStalkerTransformer * transformer;
  GumEventSink * sink;
};

struct _GumDisinfectContext
{
  GumExecCtx * exec_ctx;
  gboolean success;
};

struct _GumActivation
{
  GumExecCtx * ctx;
  gboolean pending;
  gconstpointer target;
};

struct _GumInvalidateContext
{
  GumExecBlock * block;
  gboolean is_executing_target_block;
};

struct _GumCallProbe
{
  gint ref_count;
  GumProbeId id;
  GumCallProbeCallback callback;
  gpointer user_data;
  GDestroyNotify user_notify;
};

struct _GumExecCtx
{
  volatile gint state;
  GumExecCtxMode mode;
  gint64 destroy_pending_since;

  GumStalker * stalker;
  GumThreadId thread_id;
#ifdef HAVE_WINDOWS
  GumNativeRegisterValue previous_pc;
  GumNativeRegisterValue previous_dr0;
  GumNativeRegisterValue previous_dr1;
  GumNativeRegisterValue previous_dr2;
  GumNativeRegisterValue previous_dr7;
#endif

  GumX86Writer code_writer;
  GumX86Relocator relocator;

  GumStalkerTransformer * transformer;
  void (* transform_block_impl) (GumStalkerTransformer * self,
      GumStalkerIterator * iterator, GumStalkerOutput * output);
  GumEventSink * sink;
  gboolean sink_started;
  GumEventType sink_mask;
  void (* sink_process_impl) (GumEventSink * self, const GumEvent * event,
      GumCpuContext * cpu_context);
  GumStalkerObserver * observer;

  gboolean unfollow_called_while_still_following;
  GumExecBlock * current_block;
  gpointer pending_return_location;
  guint pending_calls;
  GumExecFrame * current_frame;
  GumExecFrame * first_frame;
  GumExecFrame * frames;

  gpointer resume_at;
  gpointer return_at;
  gpointer app_stack;
  gconstpointer activation_target;

  gpointer thunks;
  gpointer infect_thunk;
  GumAddress infect_body;

  GumSpinlock code_lock;
  GumCodeSlab * code_slab;
  GumDataSlab * data_slab;
  GumCodeSlab * scratch_slab;
  GumMetalHashTable * mappings;
  gpointer last_prolog_minimal;
  gpointer last_epilog_minimal;
  gpointer last_prolog_full;
  gpointer last_epilog_full;
  gpointer last_stack_push;
  gpointer last_stack_pop_and_go;
  gpointer last_invalidator;
};

enum _GumExecCtxState
{
  GUM_EXEC_CTX_ACTIVE,
  GUM_EXEC_CTX_UNFOLLOW_PENDING,
  GUM_EXEC_CTX_DESTROY_PENDING
};

enum _GumExecCtxMode
{
  GUM_EXEC_CTX_NORMAL,
  GUM_EXEC_CTX_SINGLE_STEPPING_ON_CALL,
  GUM_EXEC_CTX_SINGLE_STEPPING_THROUGH_CALL
};

struct _GumExecBlock
{
  GumExecCtx * ctx;
  GumCodeSlab * code_slab;
  GumExecBlock * storage_block;

  guint8 * real_start;
  guint8 * code_start;
  guint real_size;
  guint code_size;
  guint capacity;
  guint last_callout_offset;

  GumExecBlockFlags flags;
  gint recycle_count;
};

enum _GumExecBlockFlags
{
  GUM_EXEC_BLOCK_ACTIVATION_TARGET = 1 << 0,
};

struct _GumExecFrame
{
  gpointer real_address;
  gpointer code_address;
};

struct _GumSlab
{
  guint8 * data;
  guint offset;
  guint size;
  GumSlab * next;
};

struct _GumCodeSlab
{
  GumSlab slab;

  gpointer invalidator;
};

struct _GumDataSlab
{
  GumSlab slab;
};

enum _GumPrologType
{
  GUM_PROLOG_NONE,
  GUM_PROLOG_IC,
  GUM_PROLOG_MINIMAL,
  GUM_PROLOG_FULL
};

enum _GumCodeContext
{
  GUM_CODE_INTERRUPTIBLE,
  GUM_CODE_UNINTERRUPTIBLE
};

struct _GumGeneratorContext
{
  GumInstruction * instruction;
  GumX86Relocator * relocator;
  GumX86Writer * code_writer;
  gpointer continuation_real_address;
  GumPrologType opened_prolog;
  guint accumulated_stack_delta;
};

struct _GumInstruction
{
  const cs_insn * ci;
  guint8 * start;
  guint8 * end;
};

struct _GumStalkerIterator
{
  GumExecCtx * exec_context;
  GumExecBlock * exec_block;
  GumGeneratorContext * generator_context;

  GumInstruction instruction;
  GumVirtualizationRequirements requirements;
};

struct _GumCalloutEntry
{
  GumStalkerCallout callout;
  gpointer data;
  GDestroyNotify data_destroy;

  gpointer pc;

  GumExecCtx * exec_context;

  GumCalloutEntry * next;
};

struct _GumBranchTarget
{
  gpointer origin_ip;

  gpointer absolute_address;
  gssize relative_offset;

  gboolean is_indirect;
  uint8_t pfx_seg;
  x86_reg base;
  x86_reg index;
  guint8 scale;
};

enum _GumBackpatchType
{
  GUM_BACKPATCH_CALL,
  GUM_BACKPATCH_RET,
  GUM_BACKPATCH_JMP,
  GUM_BACKPATCH_INLINE_CACHE,
};

struct _GumBackpatchCall
{
  gsize code_offset;
  GumPrologType opened_prolog;
  gpointer ret_real_address;
  gsize ret_code_offset;
};

struct _GumBackpatchRet
{
  gsize code_offset;
};

struct _GumBackpatchJmp
{
  gsize code_offset;
  GumPrologType opened_prolog;
};

struct _GumBackpatchInlineCache
{
  gsize ic_offset;
};

struct _GumBackpatch
{
  GumBackpatchType type;
  guint8 * to;
  guint8 * from;

  union
  {
    GumBackpatchCall call;
    GumBackpatchRet ret;
    GumBackpatchJmp jmp;
    GumBackpatchInlineCache inline_cache;
  };
};

struct _GumIcEntry
{
  gpointer real_start;
  gpointer code_start;
};

enum _GumVirtualizationRequirements
{
  GUM_REQUIRE_NOTHING         = 0,

  GUM_REQUIRE_RELOCATION      = 1 << 0,
  GUM_REQUIRE_SINGLE_STEP     = 1 << 1
};

static void gum_stalker_dispose (GObject * object);
static void gum_stalker_finalize (GObject * object);
static void gum_stalker_get_property (GObject * object, guint property_id,
    GValue * value, GParamSpec * pspec);
static void gum_stalker_set_property (GObject * object, guint property_id,
    const GValue * value, GParamSpec * pspec);

G_GNUC_INTERNAL void _gum_stalker_do_follow_me (GumStalker * self,
    GumStalkerTransformer * transformer, GumEventSink * sink,
    gpointer * ret_addr_ptr);
static void gum_stalker_infect (GumThreadId thread_id,
    GumCpuContext * cpu_context, gpointer user_data);
static void gum_stalker_disinfect (GumThreadId thread_id,
    GumCpuContext * cpu_context, gpointer user_data);
G_GNUC_INTERNAL void _gum_stalker_do_activate (GumStalker * self,
    gconstpointer target, gpointer * ret_addr_ptr);
G_GNUC_INTERNAL void _gum_stalker_do_deactivate (GumStalker * self,
    gpointer * ret_addr_ptr);
static gboolean gum_stalker_do_invalidate (GumExecCtx * ctx,
    gconstpointer address, GumActivation * activation);
static void gum_stalker_try_invalidate_block_owned_by_thread (
    GumThreadId thread_id, GumCpuContext * cpu_context, gpointer user_data);

static GumCallProbe * gum_call_probe_ref (GumCallProbe * probe);
static void gum_call_probe_unref (GumCallProbe * probe);

static GumExecCtx * gum_stalker_create_exec_ctx (GumStalker * self,
    GumThreadId thread_id, GumStalkerTransformer * transformer,
    GumEventSink * sink);
static void gum_stalker_destroy_exec_ctx (GumStalker * self, GumExecCtx * ctx);
static GumExecCtx * gum_stalker_get_exec_ctx (GumStalker * self);
static GumExecCtx * gum_stalker_find_exec_ctx_by_thread_id (GumStalker * self,
    GumThreadId thread_id);

static gsize gum_stalker_snapshot_space_needed_for (GumStalker * self,
    gsize real_size);

static void gum_stalker_thaw (GumStalker * self, gpointer code, gsize size);
static void gum_stalker_freeze (GumStalker * self, gpointer code, gsize size);

static GumExecCtx * gum_exec_ctx_new (GumStalker * self, GumThreadId thread_id,
    GumStalkerTransformer * transformer, GumEventSink * sink);
static void gum_exec_ctx_free (GumExecCtx * ctx);
static void gum_exec_ctx_dispose (GumExecCtx * ctx);
static GumCodeSlab * gum_exec_ctx_add_code_slab (GumExecCtx * ctx,
    GumCodeSlab * code_slab);
static GumDataSlab * gum_exec_ctx_add_data_slab (GumExecCtx * ctx,
    GumDataSlab * data_slab);
static void gum_exec_ctx_compute_code_address_spec (GumExecCtx * ctx,
    gsize slab_size, GumAddressSpec * spec);
static void gum_exec_ctx_compute_data_address_spec (GumExecCtx * ctx,
    gsize slab_size, GumAddressSpec * spec);
static gboolean gum_exec_ctx_maybe_unfollow (GumExecCtx * ctx,
    gpointer resume_at);
static void gum_exec_ctx_unfollow (GumExecCtx * ctx, gpointer resume_at);
static gboolean gum_exec_ctx_has_executed (GumExecCtx * ctx);
static gboolean gum_exec_ctx_contains (GumExecCtx * ctx, gconstpointer address);
static gpointer GUM_THUNK gum_exec_ctx_switch_block (GumExecCtx * ctx,
    gpointer start_address);

static GumExecBlock * gum_exec_ctx_obtain_block_for (GumExecCtx * ctx,
    gpointer real_address, gpointer * code_address);
static void gum_exec_ctx_recompile_block (GumExecCtx * ctx,
    GumExecBlock * block);
static void gum_exec_ctx_compile_block (GumExecCtx * ctx, GumExecBlock * block,
    gconstpointer input_code, gpointer output_code, GumAddress output_pc,
    guint * input_size, guint * output_size);
static void gum_exec_ctx_maybe_emit_compile_event (GumExecCtx * ctx,
    GumExecBlock * block);

static gboolean gum_stalker_iterator_is_out_of_space (
    GumStalkerIterator * self);
static gsize gum_stalker_get_ic_entry_size (GumStalker * stalker);

static void gum_stalker_invoke_callout (GumCalloutEntry * entry,
    GumCpuContext * cpu_context);

static void gum_exec_ctx_write_prolog (GumExecCtx * ctx, GumPrologType type,
    GumX86Writer * cw);
static void gum_exec_ctx_write_epilog (GumExecCtx * ctx, GumPrologType type,
    GumX86Writer * cw);

static void gum_exec_ctx_ensure_inline_helpers_reachable (GumExecCtx * ctx);
static void gum_exec_ctx_write_minimal_prolog_helper (GumExecCtx * ctx,
    GumX86Writer * cw);
static void gum_exec_ctx_write_minimal_epilog_helper (GumExecCtx * ctx,
    GumX86Writer * cw);
static void gum_exec_ctx_write_full_prolog_helper (GumExecCtx * ctx,
    GumX86Writer * cw);
static void gum_exec_ctx_write_full_epilog_helper (GumExecCtx * ctx,
    GumX86Writer * cw);
static void gum_exec_ctx_write_prolog_helper (GumExecCtx * ctx,
    GumPrologType type, GumX86Writer * cw);
static void gum_exec_ctx_write_epilog_helper (GumExecCtx * ctx,
    GumPrologType type, GumX86Writer * cw);
static void gum_exec_ctx_write_stack_push_helper (GumExecCtx * ctx,
    GumX86Writer * cw);
static void gum_exec_ctx_write_stack_pop_and_go_helper (GumExecCtx * ctx,
    GumX86Writer * cw);
static void gum_exec_ctx_write_invalidator (GumExecCtx * ctx,
    GumX86Writer * cw);
static void gum_exec_ctx_ensure_helper_reachable (GumExecCtx * ctx,
    gpointer * helper_ptr, GumExecHelperWriteFunc write);
static gboolean gum_exec_ctx_is_helper_reachable (GumExecCtx * ctx,
    gpointer * helper_ptr);

static void gum_exec_ctx_write_push_branch_target_address (GumExecCtx * ctx,
    const GumBranchTarget * target, GumGeneratorContext * gc);
static void gum_exec_ctx_load_real_register_into (GumExecCtx * ctx,
    GumCpuReg target_register, GumCpuReg source_register,
    gpointer ip, GumGeneratorContext * gc);
static void gum_exec_ctx_load_real_register_from_minimal_frame_into (
    GumExecCtx * ctx, GumCpuReg target_register, GumCpuReg source_register,
    gpointer ip, GumGeneratorContext * gc);
static void gum_exec_ctx_load_real_register_from_full_frame_into (
    GumExecCtx * ctx, GumCpuReg target_register, GumCpuReg source_register,
    gpointer ip, GumGeneratorContext * gc);
static void gum_exec_ctx_load_real_register_from_ic_frame_into (
    GumExecCtx * ctx, GumCpuReg target_register, GumCpuReg source_register,
    gpointer ip, GumGeneratorContext * gc);

static GumExecBlock * gum_exec_block_new (GumExecCtx * ctx);
static void gum_exec_block_clear (GumExecBlock * block);
static void gum_exec_block_commit (GumExecBlock * block);
static void gum_exec_block_invalidate (GumExecBlock * block);
static gpointer gum_exec_block_get_snapshot_start (GumExecBlock * block);
static GumCalloutEntry * gum_exec_block_get_last_callout_entry (
    const GumExecBlock * block);
static void gum_exec_block_set_last_callout_entry (GumExecBlock * block,
    GumCalloutEntry * entry);

static void gum_exec_block_backpatch_call (GumExecBlock * block,
    GumExecBlock * from, gsize code_offset, GumPrologType opened_prolog,
    gpointer ret_real_address, gsize ret_code_offset);
static void gum_exec_block_backpatch_jmp (GumExecBlock * block,
    GumExecBlock * from, gsize code_offset, GumPrologType opened_prolog);
static void gum_exec_block_backpatch_ret (GumExecBlock * block,
    GumExecBlock * from, gsize code_offset);
static void gum_exec_block_backpatch_inline_cache (GumExecBlock * block,
    GumExecBlock * from, gsize ic_offset);

static GumVirtualizationRequirements gum_exec_block_virtualize_branch_insn (
    GumExecBlock * block, GumGeneratorContext * gc);
static GumVirtualizationRequirements gum_exec_block_virtualize_ret_insn (
    GumExecBlock * block, GumGeneratorContext * gc);
static GumVirtualizationRequirements gum_exec_block_virtualize_sysenter_insn (
    GumExecBlock * block, GumGeneratorContext * gc);
#if GLIB_SIZEOF_VOID_P == 4 && defined (HAVE_WINDOWS)
static GumVirtualizationRequirements
    gum_exec_block_virtualize_wow64_transition (GumExecBlock * block,
    GumGeneratorContext * gc, gpointer impl);
#endif

static void gum_exec_block_write_call_invoke_code (GumExecBlock * block,
    const GumBranchTarget * target, GumGeneratorContext * gc);
static void gum_exec_block_write_jmp_transfer_code (GumExecBlock * block,
    const GumBranchTarget * target, GumExecCtxReplaceCurrentBlockFunc func,
    GumGeneratorContext * gc);
static void gum_exec_block_write_ret_transfer_code (GumExecBlock * block,
    GumGeneratorContext * gc);
static void gum_exec_block_write_single_step_transfer_code (
    GumExecBlock * block, GumGeneratorContext * gc);
#if GLIB_SIZEOF_VOID_P == 4 && !defined (HAVE_QNX)
static void gum_exec_block_write_sysenter_continuation_code (
    GumExecBlock * block, GumGeneratorContext * gc, gpointer saved_ret_addr);
#endif

static void gum_exec_block_write_call_event_code (GumExecBlock * block,
    const GumBranchTarget * target, GumGeneratorContext * gc,
    GumCodeContext cc);
static void gum_exec_block_write_ret_event_code (GumExecBlock * block,
    GumGeneratorContext * gc, GumCodeContext cc);
static void gum_exec_block_write_exec_event_code (GumExecBlock * block,
    GumGeneratorContext * gc, GumCodeContext cc);
static void gum_exec_block_write_block_event_code (GumExecBlock * block,
    GumGeneratorContext * gc, GumCodeContext cc);
static void gum_exec_block_write_unfollow_check_code (GumExecBlock * block,
    GumGeneratorContext * gc, GumCodeContext cc);

static void gum_exec_block_maybe_write_call_probe_code (GumExecBlock * block,
    GumGeneratorContext * gc);
static void gum_exec_block_write_call_probe_code (GumExecBlock * block,
    GumGeneratorContext * gc);
static void gum_exec_block_invoke_call_probes (GumExecBlock * block,
    GumCpuContext * cpu_context);

static gpointer gum_exec_block_write_inline_data (GumX86Writer * cw,
    gconstpointer data, gsize size, GumAddress * address);

static void gum_exec_block_open_prolog (GumExecBlock * block,
    GumPrologType type, GumGeneratorContext * gc);
static void gum_exec_block_close_prolog (GumExecBlock * block,
    GumGeneratorContext * gc);

static GumCodeSlab * gum_code_slab_new (GumExecCtx * ctx);
static void gum_code_slab_free (GumCodeSlab * code_slab);
static void gum_code_slab_init (GumCodeSlab * code_slab, gsize slab_size,
    gsize page_size);

static GumDataSlab * gum_data_slab_new (GumExecCtx * ctx);
static void gum_data_slab_free (GumDataSlab * data_slab);
static void gum_data_slab_init (GumDataSlab * data_slab, gsize slab_size);

static void gum_scratch_slab_init (GumCodeSlab * scratch_slab, gsize slab_size);

static void gum_slab_free (GumSlab * slab);
static void gum_slab_init (GumSlab * slab, gsize slab_size, gsize header_size);
static gsize gum_slab_available (GumSlab * self);
static gpointer gum_slab_start (GumSlab * self);
static gpointer gum_slab_end (GumSlab * self);
static gpointer gum_slab_cursor (GumSlab * self);
static gpointer gum_slab_reserve (GumSlab * self, gsize size);
static gpointer gum_slab_try_reserve (GumSlab * self, gsize size);

static void gum_write_segment_prefix (uint8_t segment, GumX86Writer * cw);

static GumCpuReg gum_cpu_meta_reg_from_real_reg (GumCpuReg reg);
static GumCpuReg gum_cpu_reg_from_capstone (x86_reg reg);
static x86_insn gum_negate_jcc (x86_insn instruction_id);

#ifdef HAVE_WINDOWS
static gboolean gum_stalker_on_exception (GumExceptionDetails * details,
    gpointer user_data);
static void gum_enable_hardware_breakpoint (GumNativeRegisterValue * dr7_reg,
    guint index);
# if GLIB_SIZEOF_VOID_P == 4
static void gum_collect_export (GArray * impls, const TCHAR * module_name,
    const gchar * export_name);
static void gum_collect_export_by_handle (GArray * impls,
    HMODULE module_handle, const gchar * export_name);
static gpointer gum_find_system_call_above_us (GumStalker * stalker,
    gpointer * start_esp);
# endif
#endif

static gpointer gum_find_thread_exit_implementation (void);
#ifdef HAVE_DARWIN
static gboolean gum_store_thread_exit_match (GumAddress address, gsize size,
    gpointer user_data);
#endif

G_DEFINE_TYPE (GumStalker, gum_stalker, G_TYPE_OBJECT)

static gpointer _gum_thread_exit_impl;

gboolean
gum_stalker_is_supported (void)
{
  return TRUE;
}

static void
gum_stalker_class_init (GumStalkerClass * klass)
{
  GObjectClass * object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = gum_stalker_dispose;
  object_class->finalize = gum_stalker_finalize;
  object_class->get_property = gum_stalker_get_property;
  object_class->set_property = gum_stalker_set_property;

  g_object_class_install_property (object_class, PROP_IC_ENTRIES,
      g_param_spec_uint ("ic-entries", "IC Entries", "Inline Cache Entries",
      2, 32, 2,
      G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  _gum_thread_exit_impl = gum_find_thread_exit_implementation ();
}

static void
gum_stalker_init (GumStalker * self)
{
  gsize page_size;

  self->exclusions = g_array_new (FALSE, FALSE, sizeof (GumMemoryRange));
  self->trust_threshold = 1;

  gum_spinlock_init (&self->probe_lock);
  self->probe_target_by_id = g_hash_table_new_full (NULL, NULL, NULL, NULL);
  self->probe_array_by_address = g_hash_table_new_full (NULL, NULL, NULL,
      (GDestroyNotify) g_ptr_array_unref);

  page_size = gum_query_page_size ();

  self->frames_size = page_size;
  g_assert (self->frames_size % sizeof (GumExecFrame) == 0);
  self->thunks_size = page_size;
  self->code_slab_size_initial =
      GUM_ALIGN_SIZE (GUM_CODE_SLAB_SIZE_INITIAL, page_size);
  self->data_slab_size_initial =
      GUM_ALIGN_SIZE (GUM_DATA_SLAB_SIZE_INITIAL, page_size);
  self->code_slab_size_dynamic =
      GUM_ALIGN_SIZE (GUM_CODE_SLAB_SIZE_DYNAMIC, page_size);
  self->data_slab_size_dynamic =
      GUM_ALIGN_SIZE (GUM_DATA_SLAB_SIZE_DYNAMIC, page_size);
  self->scratch_slab_size = GUM_ALIGN_SIZE (GUM_SCRATCH_SLAB_SIZE, page_size);
  self->ctx_header_size = GUM_ALIGN_SIZE (sizeof (GumExecCtx), page_size);
  self->ctx_size =
      self->ctx_header_size +
      self->frames_size +
      self->thunks_size +
      self->code_slab_size_initial +
      self->data_slab_size_initial +
      self->scratch_slab_size +
      0;

  self->frames_offset = self->ctx_header_size;
  self->thunks_offset = self->frames_offset + self->frames_size;
  self->code_slab_offset = self->thunks_offset + self->thunks_size;
  self->data_slab_offset =
      self->code_slab_offset + self->code_slab_size_initial;
  self->scratch_slab_offset =
      self->data_slab_offset + self->data_slab_size_initial;

  self->page_size = page_size;
  self->cpu_features = gum_query_cpu_features ();
  self->is_rwx_supported = gum_query_rwx_support () != GUM_RWX_NONE;

  g_mutex_init (&self->mutex);
  self->contexts = NULL;
  self->exec_ctx = gum_tls_key_new ();

#ifdef HAVE_WINDOWS
  self->exceptor = gum_exceptor_obtain ();
  gum_exceptor_add (self->exceptor, gum_stalker_on_exception, self);

# if GLIB_SIZEOF_VOID_P == 4
  {
    HMODULE ntmod, usermod;
    MODULEINFO mi;
    BOOL success;
    gboolean found_user32_code;
    guint8 * p;
    GArray * impls;

    ntmod = GetModuleHandle (_T ("ntdll.dll"));
    usermod = GetModuleHandle (_T ("user32.dll"));
    g_assert (ntmod != NULL && usermod != NULL);

    success = GetModuleInformation (GetCurrentProcess (), usermod,
        &mi, sizeof (mi));
    g_assert (success);
    self->user32_start = mi.lpBaseOfDll;
    self->user32_end = (guint8 *) mi.lpBaseOfDll + mi.SizeOfImage;

    found_user32_code = FALSE;
    for (p = self->user32_start; p < (guint8 *) self->user32_end;)
    {
      MEMORY_BASIC_INFORMATION mbi;

      success = VirtualQuery (p, &mbi, sizeof (mbi)) == sizeof (mbi);
      g_assert (success);

      if (mbi.Protect == PAGE_EXECUTE_READ ||
          mbi.Protect == PAGE_EXECUTE_READWRITE ||
          mbi.Protect == PAGE_EXECUTE_WRITECOPY)
      {
        self->user32_start = mbi.BaseAddress;
        self->user32_end = (guint8 *) mbi.BaseAddress + mbi.RegionSize;

        found_user32_code = TRUE;
      }

      p = (guint8 *) mbi.BaseAddress + mbi.RegionSize;
    }
    g_assert (found_user32_code);

    self->ki_user_callback_dispatcher_impl = GUM_FUNCPTR_TO_POINTER (
        GetProcAddress (ntmod, "KiUserCallbackDispatcher"));
    g_assert (self->ki_user_callback_dispatcher_impl != NULL);

    impls = g_array_sized_new (FALSE, FALSE, sizeof (gpointer), 5);
    self->wow_transition_impls = impls;
    gum_collect_export_by_handle (impls, ntmod, "Wow64Transition");
    gum_collect_export_by_handle (impls, usermod, "Wow64Transition");
    gum_collect_export (impls, _T ("kernel32.dll"), "Wow64Transition");
    gum_collect_export (impls, _T ("kernelbase.dll"), "Wow64Transition");
    gum_collect_export (impls, _T ("win32u.dll"), "Wow64Transition");
  }
# endif
#endif
}

static void
gum_stalker_dispose (GObject * object)
{
#ifdef HAVE_WINDOWS
  GumStalker * self = GUM_STALKER (object);

  if (self->exceptor != NULL)
  {
    gum_exceptor_remove (self->exceptor, gum_stalker_on_exception, self);
    g_object_unref (self->exceptor);
    self->exceptor = NULL;
  }
#endif

  G_OBJECT_CLASS (gum_stalker_parent_class)->dispose (object);
}

static void
gum_stalker_finalize (GObject * object)
{
  GumStalker * self = GUM_STALKER (object);

#if defined (HAVE_WINDOWS) && GLIB_SIZEOF_VOID_P == 4
  g_array_unref (self->wow_transition_impls);
#endif

  g_hash_table_unref (self->probe_array_by_address);
  g_hash_table_unref (self->probe_target_by_id);

  g_array_free (self->exclusions, TRUE);

  g_assert (self->contexts == NULL);
  gum_tls_key_free (self->exec_ctx);
  g_mutex_clear (&self->mutex);

  G_OBJECT_CLASS (gum_stalker_parent_class)->finalize (object);
}

static void
gum_stalker_get_property (GObject * object,
                          guint property_id,
                          GValue * value,
                          GParamSpec * pspec)
{
  GumStalker * self = GUM_STALKER (object);

  switch (property_id)
  {
    case PROP_IC_ENTRIES:
      g_value_set_uint (value, self->ic_entries);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

static void
gum_stalker_set_property (GObject * object,
                          guint property_id,
                          const GValue * value,
                          GParamSpec * pspec)
{
  GumStalker * self = GUM_STALKER (object);

  switch (property_id)
  {
    case PROP_IC_ENTRIES:
      self->ic_entries = g_value_get_uint (value);
      break;
    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
  }
}

GumStalker *
gum_stalker_new (void)
{
  return g_object_new (GUM_TYPE_STALKER, NULL);
}

void
gum_stalker_exclude (GumStalker * self,
                     const GumMemoryRange * range)
{
  g_array_append_val (self->exclusions, *range);
}

static gboolean
gum_stalker_is_excluding (GumStalker * self,
                          gconstpointer address)
{
  GArray * exclusions = self->exclusions;
  guint i;

  for (i = 0; i != exclusions->len; i++)
  {
    GumMemoryRange * r = &g_array_index (exclusions, GumMemoryRange, i);

    if (GUM_MEMORY_RANGE_INCLUDES (r, GUM_ADDRESS (address)))
      return TRUE;
  }

  return FALSE;
}

gint
gum_stalker_get_trust_threshold (GumStalker * self)
{
  return self->trust_threshold;
}

void
gum_stalker_set_trust_threshold (GumStalker * self,
                                 gint trust_threshold)
{
  self->trust_threshold = trust_threshold;
}

void
gum_stalker_flush (GumStalker * self)
{
  GSList * sinks, * cur;

  GUM_STALKER_LOCK (self);

  sinks = NULL;
  for (cur = self->contexts; cur != NULL; cur = cur->next)
  {
    GumExecCtx * ctx = cur->data;

    sinks = g_slist_prepend (sinks, g_object_ref (ctx->sink));
  }

  GUM_STALKER_UNLOCK (self);

  for (cur = sinks; cur != NULL; cur = cur->next)
  {
    GumEventSink * sink = cur->data;

    gum_event_sink_flush (sink);
  }

  g_slist_free_full (sinks, g_object_unref);
}

void
gum_stalker_stop (GumStalker * self)
{
  GSList * cur;

  gum_spinlock_acquire (&self->probe_lock);
  g_hash_table_remove_all (self->probe_target_by_id);
  g_hash_table_remove_all (self->probe_array_by_address);
  self->any_probes_attached = FALSE;
  gum_spinlock_release (&self->probe_lock);

rescan:
  GUM_STALKER_LOCK (self);

  for (cur = self->contexts; cur != NULL; cur = cur->next)
  {
    GumExecCtx * ctx = cur->data;

    if (g_atomic_int_get (&ctx->state) == GUM_EXEC_CTX_ACTIVE)
    {
      GumThreadId thread_id = ctx->thread_id;

      GUM_STALKER_UNLOCK (self);

      gum_stalker_unfollow (self, thread_id);

      goto rescan;
    }
  }

  GUM_STALKER_UNLOCK (self);

  gum_stalker_garbage_collect (self);
}

gboolean
gum_stalker_garbage_collect (GumStalker * self)
{
  gboolean have_pending_garbage;
  GumThreadId current_thread_id;
  gint64 now;
  GSList * cur;

  current_thread_id = gum_process_get_current_thread_id ();
  now = g_get_monotonic_time ();

rescan:
  GUM_STALKER_LOCK (self);

  for (cur = self->contexts; cur != NULL; cur = cur->next)
  {
    GumExecCtx * ctx = cur->data;
    gboolean destroy_pending_and_thread_likely_back_in_original_code;

    destroy_pending_and_thread_likely_back_in_original_code =
        g_atomic_int_get (&ctx->state) == GUM_EXEC_CTX_DESTROY_PENDING &&
        (ctx->thread_id == current_thread_id ||
        now - ctx->destroy_pending_since > 20000);

    if (destroy_pending_and_thread_likely_back_in_original_code ||
        !gum_process_has_thread (ctx->thread_id))
    {
      GUM_STALKER_UNLOCK (self);

      gum_stalker_destroy_exec_ctx (self, ctx);

      goto rescan;
    }
  }

  have_pending_garbage = self->contexts != NULL;

  GUM_STALKER_UNLOCK (self);

  return have_pending_garbage;
}

#ifdef _MSC_VER

#define RETURN_ADDRESS_POINTER_FROM_FIRST_ARGUMENT(arg)   \
    ((gpointer *) ((volatile guint8 *) &arg - sizeof (gpointer)))

GUM_NOINLINE void
gum_stalker_follow_me (GumStalker * self,
                       GumStalkerTransformer * transformer,
                       GumEventSink * sink)
{
  gpointer * ret_addr_ptr;

  ret_addr_ptr = RETURN_ADDRESS_POINTER_FROM_FIRST_ARGUMENT (self);

  _gum_stalker_do_follow_me (self, transformer, sink, ret_addr_ptr);
}

#endif

void
_gum_stalker_do_follow_me (GumStalker * self,
                           GumStalkerTransformer * transformer,
                           GumEventSink * sink,
                           gpointer * ret_addr_ptr)
{
  GumExecCtx * ctx;
  gpointer code_address;

  ctx = gum_stalker_create_exec_ctx (self, gum_process_get_current_thread_id (),
      transformer, sink);
  gum_tls_key_set_value (self->exec_ctx, ctx);

  ctx->current_block = gum_exec_ctx_obtain_block_for (ctx, *ret_addr_ptr,
      &code_address);

  if (gum_exec_ctx_maybe_unfollow (ctx, *ret_addr_ptr))
  {
    gum_stalker_destroy_exec_ctx (self, ctx);
    return;
  }

  gum_event_sink_start (ctx->sink);
  ctx->sink_started = TRUE;

  *ret_addr_ptr = code_address;
}

GUM_NOINLINE void
gum_stalker_unfollow_me (GumStalker * self)
{
  GumExecCtx * ctx;

  ctx = gum_stalker_get_exec_ctx (self);
  if (ctx == NULL)
    return;

  g_atomic_int_set (&ctx->state, GUM_EXEC_CTX_UNFOLLOW_PENDING);

  if (!gum_exec_ctx_maybe_unfollow (ctx, NULL))
    return;

  g_assert (ctx->unfollow_called_while_still_following);

  gum_stalker_destroy_exec_ctx (self, ctx);
}

gboolean
gum_stalker_is_following_me (GumStalker * self)
{
  return gum_stalker_get_exec_ctx (self) != NULL;
}

void
gum_stalker_follow (GumStalker * self,
                    GumThreadId thread_id,
                    GumStalkerTransformer * transformer,
                    GumEventSink * sink)
{
  if (thread_id == gum_process_get_current_thread_id ())
  {
    gum_stalker_follow_me (self, transformer, sink);
  }
  else
  {
    GumInfectContext ctx;

    ctx.stalker = self;
    ctx.transformer = transformer;
    ctx.sink = sink;

    gum_process_modify_thread (thread_id, gum_stalker_infect, &ctx);
  }
}

void
gum_stalker_unfollow (GumStalker * self,
                      GumThreadId thread_id)
{
  if (thread_id == gum_process_get_current_thread_id ())
  {
    gum_stalker_unfollow_me (self);
  }
  else
  {
    GumExecCtx * ctx;

    ctx = gum_stalker_find_exec_ctx_by_thread_id (self, thread_id);
    if (ctx == NULL)
      return;

    if (!g_atomic_int_compare_and_exchange (&ctx->state, GUM_EXEC_CTX_ACTIVE,
        GUM_EXEC_CTX_UNFOLLOW_PENDING))
      return;

    if (!gum_exec_ctx_has_executed (ctx))
    {
      GumDisinfectContext dc;

      dc.exec_ctx = ctx;
      dc.success = FALSE;

      gum_process_modify_thread (thread_id, gum_stalker_disinfect, &dc);

      if (dc.success)
        gum_stalker_destroy_exec_ctx (self, ctx);
    }
  }
}

static void
gum_stalker_infect (GumThreadId thread_id,
                    GumCpuContext * cpu_context,
                    gpointer user_data)
{
  GumInfectContext * infect_context = (GumInfectContext *) user_data;
  GumStalker * self = infect_context->stalker;
  GumExecCtx * ctx;
  guint8 * pc;
  const guint max_syscall_size = 2;
  gpointer code_address;
  GumX86Writer * cw;

  ctx = gum_stalker_create_exec_ctx (self, thread_id,
      infect_context->transformer, infect_context->sink);

  pc = GSIZE_TO_POINTER (GUM_CPU_CONTEXT_XIP (cpu_context));

  ctx->current_block = gum_exec_ctx_obtain_block_for (ctx, pc, &code_address);

  if (gum_exec_ctx_maybe_unfollow (ctx, NULL))
  {
    gum_stalker_destroy_exec_ctx (self, ctx);
    return;
  }

  gum_spinlock_acquire (&ctx->code_lock);

  gum_stalker_thaw (self, ctx->thunks, self->thunks_size);
  cw = &ctx->code_writer;
  gum_x86_writer_reset (cw, ctx->infect_thunk);

  /*
   * In case the thread is in a Linux system call we should allow it to be
   * restarted by bringing along the syscall instruction.
   */
  gum_x86_writer_put_bytes (cw, pc - max_syscall_size, max_syscall_size);

  ctx->infect_body = GUM_ADDRESS (gum_x86_writer_cur (cw));
  gum_exec_ctx_write_prolog (ctx, GUM_PROLOG_MINIMAL, cw);
  gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
      GUM_ADDRESS (gum_tls_key_set_value), 2,
      GUM_ARG_ADDRESS, GUM_ADDRESS (self->exec_ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (ctx));
  gum_exec_ctx_write_epilog (ctx, GUM_PROLOG_MINIMAL, cw);

  gum_x86_writer_put_jmp_address (cw, GUM_ADDRESS (code_address));

  gum_x86_writer_flush (cw);
  gum_stalker_freeze (self, cw->base, gum_x86_writer_offset (cw));

  gum_spinlock_release (&ctx->code_lock);

  gum_event_sink_start (ctx->sink);

#ifdef HAVE_WINDOWS
  {
    gboolean probably_in_syscall;

    probably_in_syscall =
# if GLIB_SIZEOF_VOID_P == 8
        pc[0] == 0xc3 && pc[-2] == 0x0f && pc[-1] == 0x05;
# else
        (pc[0] == 0xc2 || pc[0] == 0xc3) &&
            pc[-2] == 0xff && (pc[-1] & 0xf8) == 0xd0;
# endif
    if (probably_in_syscall)
    {
      gboolean breakpoint_deployed = FALSE;
      HANDLE thread;

      thread = OpenThread (THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
          thread_id);
      if (thread != NULL)
      {
        __declspec (align (64)) CONTEXT tc = { 0, };

        tc.ContextFlags = CONTEXT_DEBUG_REGISTERS;
        if (GetThreadContext (thread, &tc))
        {
          ctx->previous_pc = GPOINTER_TO_SIZE (pc);
          ctx->previous_dr0 = tc.Dr0;
          ctx->previous_dr7 = tc.Dr7;

          tc.Dr0 = GPOINTER_TO_SIZE (pc);
          tc.Dr7 = 0x00000700;
          gum_enable_hardware_breakpoint (&tc.Dr7, 0);

          breakpoint_deployed = SetThreadContext (thread, &tc);
        }

        CloseHandle (thread);
      }

      if (!breakpoint_deployed)
        gum_stalker_destroy_exec_ctx (self, ctx);

      return;
    }
  }
#endif

  GUM_CPU_CONTEXT_XIP (cpu_context) = ctx->infect_body;
}

static void
gum_stalker_disinfect (GumThreadId thread_id,
                       GumCpuContext * cpu_context,
                       gpointer user_data)
{
  GumDisinfectContext * disinfect_context = user_data;
  GumExecCtx * ctx = disinfect_context->exec_ctx;
  gboolean infection_not_active_yet;

#ifdef HAVE_WINDOWS
  infection_not_active_yet =
      GUM_CPU_CONTEXT_XIP (cpu_context) == ctx->previous_pc;
  if (infection_not_active_yet)
  {
    HANDLE thread;

    thread = OpenThread (THREAD_GET_CONTEXT | THREAD_SET_CONTEXT, FALSE,
        thread_id);
    if (thread != NULL)
    {
      __declspec (align (64)) CONTEXT tc = { 0, };

      tc.ContextFlags = CONTEXT_DEBUG_REGISTERS;
      if (GetThreadContext (thread, &tc))
      {
        tc.Dr0 = ctx->previous_dr0;
        tc.Dr7 = ctx->previous_dr7;

        ctx->previous_pc = 0;

        disinfect_context->success = SetThreadContext (thread, &tc);
      }

      CloseHandle (thread);
    }
  }
#else
  infection_not_active_yet =
      GUM_CPU_CONTEXT_XIP (cpu_context) == ctx->infect_body;
  if (infection_not_active_yet)
  {
    GUM_CPU_CONTEXT_XIP (cpu_context) =
        GPOINTER_TO_SIZE (ctx->current_block->real_start);

    disinfect_context->success = TRUE;
  }
#endif
}

#ifdef _MSC_VER

GUM_NOINLINE void
gum_stalker_activate (GumStalker * self,
                      gconstpointer target)
{
  gpointer * ret_addr_ptr;

  ret_addr_ptr = RETURN_ADDRESS_POINTER_FROM_FIRST_ARGUMENT (self);

  _gum_stalker_do_activate (self, target, ret_addr_ptr);
}

GUM_NOINLINE void
gum_stalker_deactivate (GumStalker * self)
{
  gpointer * ret_addr_ptr;

  ret_addr_ptr = RETURN_ADDRESS_POINTER_FROM_FIRST_ARGUMENT (self);

  _gum_stalker_do_deactivate (self, ret_addr_ptr);
}

#endif

void
_gum_stalker_do_activate (GumStalker * self,
                          gconstpointer target,
                          gpointer * ret_addr_ptr)
{
  guint8 * ret_addr = *ret_addr_ptr;
  GumExecCtx * ctx;

  ctx = gum_stalker_get_exec_ctx (self);
  if (ctx == NULL)
    return;

  ctx->unfollow_called_while_still_following = FALSE;
  ctx->activation_target = target;

  if (!gum_exec_ctx_contains (ctx, ret_addr))
  {
    gpointer code_address;

    ctx->current_block =
        gum_exec_ctx_obtain_block_for (ctx, ret_addr, &code_address);

    if (gum_exec_ctx_maybe_unfollow (ctx, ret_addr))
      return;

    *ret_addr_ptr = code_address;
  }
}

void
_gum_stalker_do_deactivate (GumStalker * self,
                            gpointer * ret_addr_ptr)
{
  GumExecCtx * ctx;

  ctx = gum_stalker_get_exec_ctx (self);
  if (ctx == NULL)
    return;

  ctx->unfollow_called_while_still_following = TRUE;
  ctx->activation_target = NULL;

  if (gum_exec_ctx_contains (ctx, *ret_addr_ptr))
  {
    ctx->pending_calls--;

    *ret_addr_ptr = ctx->pending_return_location;
  }
}

static void
gum_stalker_maybe_deactivate (GumStalker * self,
                              GumActivation * activation)
{
  GumExecCtx * ctx;

  ctx = gum_stalker_get_exec_ctx (self);
  activation->ctx = ctx;

  if (ctx != NULL && ctx->pending_calls == 0)
  {
    activation->pending = TRUE;
    activation->target = ctx->activation_target;

    gum_stalker_deactivate (self);
  }
  else
  {
    activation->pending = FALSE;
    activation->target = NULL;
  }
}

static void
gum_stalker_maybe_reactivate (GumStalker * self,
                              GumActivation * activation)
{
  if (activation->pending)
    gum_stalker_activate (self, activation->target);
}

void
gum_stalker_set_observer (GumStalker * self,
                          GumStalkerObserver * observer)
{
  GumExecCtx * ctx;

  ctx = gum_stalker_get_exec_ctx (self);
  g_assert (ctx != NULL);

  if (observer != NULL)
    g_object_ref (observer);
  if (ctx->observer != NULL)
    g_object_unref (ctx->observer);
  ctx->observer = observer;
}

void
gum_stalker_prefetch (GumStalker * self,
                      gconstpointer address,
                      gint recycle_count)
{
  GumExecCtx * ctx;
  GumExecBlock * block;
  gpointer code_address;

  ctx = gum_stalker_get_exec_ctx (self);
  g_assert (ctx != NULL);

  block = gum_exec_ctx_obtain_block_for (ctx, (gpointer) address,
      &code_address);
  block->recycle_count = recycle_count;
}

void
gum_stalker_prefetch_backpatch (GumStalker * self,
                                const GumBackpatch * backpatch)
{
  GumExecCtx * ctx;
  GumExecBlock * block_to, * block_from;
  gpointer code_address_to, code_address_from;

  ctx = gum_stalker_get_exec_ctx (self);
  g_assert (ctx != NULL);

  block_to = gum_exec_ctx_obtain_block_for (ctx, backpatch->to,
      &code_address_to);
  block_from = gum_exec_ctx_obtain_block_for (ctx, backpatch->from,
      &code_address_from);

  block_to->recycle_count = self->trust_threshold;
  block_from->recycle_count = self->trust_threshold;

  switch (backpatch->type)
  {
    case GUM_BACKPATCH_CALL:
    {
      const GumBackpatchCall * call = &backpatch->call;
      gum_exec_block_backpatch_call (block_to, block_from, call->code_offset,
          call->opened_prolog, call->ret_real_address, call->ret_code_offset);
      break;
    }
    case GUM_BACKPATCH_RET:
    {
      const GumBackpatchRet * ret = &backpatch->ret;
      gum_exec_block_backpatch_ret (block_to, block_from, ret->code_offset);
      break;
    }
    case GUM_BACKPATCH_JMP:
    {
      const GumBackpatchJmp * jmp = &backpatch->jmp;
      gum_exec_block_backpatch_jmp (block_to, block_from, jmp->code_offset,
          jmp->opened_prolog);
      break;
    }
    case GUM_BACKPATCH_INLINE_CACHE:
    {
      const GumBackpatchInlineCache * ic = &backpatch->inline_cache;
      gum_exec_block_backpatch_inline_cache (block_to, block_from,
          ic->ic_offset);
      break;
    }
    default:
      g_assert_not_reached ();
      break;
  }
}

void
gum_stalker_invalidate (GumStalker * self,
                        gconstpointer address)
{
  GumActivation activation;

  gum_stalker_maybe_deactivate (self, &activation);
  if (activation.ctx == NULL)
    return;

  gum_stalker_do_invalidate (activation.ctx, address, &activation);

  gum_stalker_maybe_reactivate (self, &activation);
}

void
gum_stalker_invalidate_for_thread (GumStalker * self,
                                   GumThreadId thread_id,
                                   gconstpointer address)
{
  GumActivation activation;
  GumExecCtx * ctx;

  gum_stalker_maybe_deactivate (self, &activation);

  ctx = gum_stalker_find_exec_ctx_by_thread_id (self, thread_id);
  if (ctx != NULL)
  {
    while (!gum_stalker_do_invalidate (ctx, address, &activation))
    {
      g_thread_yield ();
    }
  }

  gum_stalker_maybe_reactivate (self, &activation);
}

static void
gum_stalker_invalidate_for_all_threads (GumStalker * self,
                                        gconstpointer address,
                                        GumActivation * activation)
{
  GSList * contexts, * cur;

  GUM_STALKER_LOCK (self);
  contexts = g_slist_copy (self->contexts);
  GUM_STALKER_UNLOCK (self);

  cur = contexts;

  while (cur != NULL)
  {
    GumExecCtx * ctx = cur->data;
    GSList * l;

    if (!gum_stalker_do_invalidate (ctx, address, activation))
    {
      cur = g_slist_append (cur, ctx);
    }

    l = cur;
    cur = cur->next;
    g_slist_free_1 (l);
  }
}

static gboolean
gum_stalker_do_invalidate (GumExecCtx * ctx,
                           gconstpointer address,
                           GumActivation * activation)
{
  GumInvalidateContext ic;

  ic.is_executing_target_block = FALSE;

  gum_spinlock_acquire (&ctx->code_lock);

  if ((ic.block = gum_metal_hash_table_lookup (ctx->mappings, address)) != NULL)
  {
    if (ctx == activation->ctx)
    {
      gum_exec_block_invalidate (ic.block);
    }
    else
    {
      gum_process_modify_thread (ctx->thread_id,
          gum_stalker_try_invalidate_block_owned_by_thread, &ic);
    }
  }

  gum_spinlock_release (&ctx->code_lock);

  return !ic.is_executing_target_block;
}

static void
gum_stalker_try_invalidate_block_owned_by_thread (GumThreadId thread_id,
                                                  GumCpuContext * cpu_context,
                                                  gpointer user_data)
{
  GumInvalidateContext * ic = user_data;
  GumExecBlock * block = ic->block;
  const guint8 * pc = GSIZE_TO_POINTER (GUM_CPU_CONTEXT_XIP (cpu_context));

  if (pc >= block->code_start &&
      pc < block->code_start + GUM_INVALIDATE_TRAMPOLINE_SIZE)
  {
    ic->is_executing_target_block = TRUE;
    return;
  }

  gum_exec_block_invalidate (block);
}

GumProbeId
gum_stalker_add_call_probe (GumStalker * self,
                            gpointer target_address,
                            GumCallProbeCallback callback,
                            gpointer data,
                            GDestroyNotify notify)
{
  GumActivation activation;
  GumCallProbe * probe;
  GPtrArray * probes;
  gboolean is_first_for_target;

  gum_stalker_maybe_deactivate (self, &activation);

  target_address = gum_strip_code_pointer (target_address);
  is_first_for_target = FALSE;

  probe = g_slice_new (GumCallProbe);
  probe->ref_count = 1;
  probe->id = g_atomic_int_add (&self->last_probe_id, 1) + 1;
  probe->callback = callback;
  probe->user_data = data;
  probe->user_notify = notify;

  gum_spinlock_acquire (&self->probe_lock);

  g_hash_table_insert (self->probe_target_by_id, GSIZE_TO_POINTER (probe->id),
      target_address);

  probes = g_hash_table_lookup (self->probe_array_by_address, target_address);
  if (probes == NULL)
  {
    probes =
        g_ptr_array_new_with_free_func ((GDestroyNotify) gum_call_probe_unref);
    g_hash_table_insert (self->probe_array_by_address, target_address, probes);

    is_first_for_target = TRUE;
  }

  g_ptr_array_add (probes, probe);

  self->any_probes_attached = TRUE;

  gum_spinlock_release (&self->probe_lock);

  if (is_first_for_target)
    gum_stalker_invalidate_for_all_threads (self, target_address, &activation);

  gum_stalker_maybe_reactivate (self, &activation);

  return probe->id;
}

void
gum_stalker_remove_call_probe (GumStalker * self,
                               GumProbeId id)
{
  GumActivation activation;
  gpointer target_address;
  gboolean is_last_for_target;

  gum_stalker_maybe_deactivate (self, &activation);

  gum_spinlock_acquire (&self->probe_lock);

  target_address =
      g_hash_table_lookup (self->probe_target_by_id, GSIZE_TO_POINTER (id));
  is_last_for_target = FALSE;

  if (target_address != NULL)
  {
    GPtrArray * probes;
    gint match_index = -1;
    guint i;

    g_hash_table_remove (self->probe_target_by_id, GSIZE_TO_POINTER (id));

    probes = g_hash_table_lookup (self->probe_array_by_address, target_address);
    g_assert (probes != NULL);

    for (i = 0; i != probes->len; i++)
    {
      GumCallProbe * probe = g_ptr_array_index (probes, i);
      if (probe->id == id)
      {
        match_index = i;
        break;
      }
    }
    g_assert (match_index != -1);

    g_ptr_array_remove_index (probes, match_index);

    if (probes->len == 0)
    {
      g_hash_table_remove (self->probe_array_by_address, target_address);

      is_last_for_target = TRUE;
    }

    self->any_probes_attached =
        g_hash_table_size (self->probe_array_by_address) != 0;
  }

  gum_spinlock_release (&self->probe_lock);

  if (is_last_for_target)
    gum_stalker_invalidate_for_all_threads (self, target_address, &activation);

  gum_stalker_maybe_reactivate (self, &activation);
}

static void
gum_call_probe_finalize (GumCallProbe * probe)
{
  if (probe->user_notify != NULL)
    probe->user_notify (probe->user_data);
}

static GumCallProbe *
gum_call_probe_ref (GumCallProbe * probe)
{
  g_atomic_int_inc (&probe->ref_count);

  return probe;
}

static void
gum_call_probe_unref (GumCallProbe * probe)
{
  if (g_atomic_int_dec_and_test (&probe->ref_count))
  {
    gum_call_probe_finalize (probe);
  }
}

static GumExecCtx *
gum_stalker_create_exec_ctx (GumStalker * self,
                             GumThreadId thread_id,
                             GumStalkerTransformer * transformer,
                             GumEventSink * sink)
{
  GumExecCtx * ctx = gum_exec_ctx_new (self, thread_id, transformer, sink);

  GUM_STALKER_LOCK (self);
  self->contexts = g_slist_prepend (self->contexts, ctx);
  GUM_STALKER_UNLOCK (self);

  return ctx;
}

static void
gum_stalker_destroy_exec_ctx (GumStalker * self,
                              GumExecCtx * ctx)
{
  GSList * entry;

  GUM_STALKER_LOCK (self);
  entry = g_slist_find (self->contexts, ctx);
  if (entry != NULL)
    self->contexts = g_slist_delete_link (self->contexts, entry);
  GUM_STALKER_UNLOCK (self);

  /* Racy due to garbage-collection. */
  if (entry == NULL)
    return;

  gum_exec_ctx_dispose (ctx);

  if (ctx->sink_started)
  {
    gum_event_sink_stop (ctx->sink);

    ctx->sink_started = FALSE;
  }

  gum_exec_ctx_free (ctx);
}

static GumExecCtx *
gum_stalker_get_exec_ctx (GumStalker * self)
{
  return gum_tls_key_get_value (self->exec_ctx);
}

static GumExecCtx *
gum_stalker_find_exec_ctx_by_thread_id (GumStalker * self,
                                        GumThreadId thread_id)
{
  GumExecCtx * ctx = NULL;
  GSList * cur;

  GUM_STALKER_LOCK (self);

  for (cur = self->contexts; cur != NULL; cur = cur->next)
  {
    GumExecCtx * candidate = cur->data;

    if (candidate->thread_id == thread_id)
    {
      ctx = candidate;
      break;
    }
  }

  GUM_STALKER_UNLOCK (self);

  return ctx;
}

static gsize
gum_stalker_snapshot_space_needed_for (GumStalker * self,
                                       gsize real_size)
{
  return (self->trust_threshold != 0) ? real_size : 0;
}

static void
gum_stalker_thaw (GumStalker * self,
                  gpointer code,
                  gsize size)
{
  if (!self->is_rwx_supported)
    gum_mprotect (code, size, GUM_PAGE_RW);
}

static void
gum_stalker_freeze (GumStalker * self,
                    gpointer code,
                    gsize size)
{
  if (!self->is_rwx_supported)
    gum_memory_mark_code (code, size);

  gum_clear_cache (code, size);
}

static GumExecCtx *
gum_exec_ctx_new (GumStalker * stalker,
                  GumThreadId thread_id,
                  GumStalkerTransformer * transformer,
                  GumEventSink * sink)
{
  GumExecCtx * ctx;
  guint8 * base;
  GumCodeSlab * code_slab;
  GumDataSlab * data_slab;

  base = gum_memory_allocate (NULL, stalker->ctx_size, stalker->page_size,
      stalker->is_rwx_supported ? GUM_PAGE_RWX : GUM_PAGE_RW);

  ctx = (GumExecCtx *) base;

  ctx->state = GUM_EXEC_CTX_ACTIVE;
  ctx->mode = GUM_EXEC_CTX_NORMAL;

  ctx->stalker = g_object_ref (stalker);
  ctx->thread_id = thread_id;

  gum_x86_writer_init (&ctx->code_writer, NULL);
  gum_x86_relocator_init (&ctx->relocator, NULL, &ctx->code_writer);

  if (transformer != NULL)
    ctx->transformer = g_object_ref (transformer);
  else
    ctx->transformer = gum_stalker_transformer_make_default ();
  ctx->transform_block_impl =
      GUM_STALKER_TRANSFORMER_GET_IFACE (ctx->transformer)->transform_block;

  if (sink != NULL)
    ctx->sink = g_object_ref (sink);
  else
    ctx->sink = gum_event_sink_make_default ();

  ctx->sink_mask = gum_event_sink_query_mask (ctx->sink);
  ctx->sink_process_impl = GUM_EVENT_SINK_GET_IFACE (ctx->sink)->process;

  ctx->observer = NULL;

  ctx->frames = (GumExecFrame *) (base + stalker->frames_offset);
  ctx->first_frame =
      ctx->frames + (stalker->frames_size / sizeof (GumExecFrame)) - 1;
  ctx->current_frame = ctx->first_frame;

  ctx->thunks = base + stalker->thunks_offset;
  ctx->infect_thunk = ctx->thunks;

  gum_spinlock_init (&ctx->code_lock);

  code_slab = (GumCodeSlab *) (base + stalker->code_slab_offset);
  gum_code_slab_init (code_slab, stalker->code_slab_size_initial,
      stalker->page_size);
  gum_exec_ctx_add_code_slab (ctx, code_slab);

  data_slab = (GumDataSlab *) (base + stalker->data_slab_offset);
  gum_data_slab_init (data_slab, stalker->data_slab_size_initial);
  gum_exec_ctx_add_data_slab (ctx, data_slab);

  ctx->scratch_slab = (GumCodeSlab *) (base + stalker->scratch_slab_offset);
  gum_scratch_slab_init (ctx->scratch_slab, stalker->scratch_slab_size);

  ctx->mappings = gum_metal_hash_table_new (NULL, NULL);

  gum_exec_ctx_ensure_inline_helpers_reachable (ctx);

  code_slab->invalidator = ctx->last_invalidator;

  return ctx;
}

static void
gum_exec_ctx_free (GumExecCtx * ctx)
{
  GumStalker * stalker = ctx->stalker;
  GumDataSlab * data_slab;
  GumCodeSlab * code_slab;

  gum_metal_hash_table_unref (ctx->mappings);

  data_slab = ctx->data_slab;
  while (TRUE)
  {
    GumDataSlab * next = (GumDataSlab *) data_slab->slab.next;
    gboolean is_initial;

    is_initial = next == NULL;
    if (is_initial)
      break;

    gum_data_slab_free (data_slab);

    data_slab = next;
  }

  code_slab = ctx->code_slab;
  while (TRUE)
  {
    GumCodeSlab * next = (GumCodeSlab *) code_slab->slab.next;
    gboolean is_initial;

    is_initial = next == NULL;
    if (is_initial)
      break;

    gum_code_slab_free (code_slab);

    code_slab = next;
  }

  g_object_unref (ctx->sink);
  g_object_unref (ctx->transformer);
  g_clear_object (&ctx->observer);

  gum_x86_relocator_clear (&ctx->relocator);
  gum_x86_writer_clear (&ctx->code_writer);

  g_object_unref (stalker);

  gum_memory_free (ctx, stalker->ctx_size);
}

static void
gum_exec_ctx_dispose (GumExecCtx * ctx)
{
  GumStalker * stalker = ctx->stalker;
  GumSlab * slab;

  for (slab = &ctx->code_slab->slab; slab != NULL; slab = slab->next)
  {
    gum_stalker_thaw (stalker, gum_slab_start (slab), slab->offset);
  }

  for (slab = &ctx->data_slab->slab; slab != NULL; slab = slab->next)
  {
    GumExecBlock * blocks;
    guint num_blocks;
    guint i;

    blocks = gum_slab_start (slab);
    num_blocks = slab->offset / sizeof (GumExecBlock);

    for (i = 0; i != num_blocks; i++)
    {
      GumExecBlock * block = &blocks[i];

      gum_exec_block_clear (block);
    }
  }
}

static GumCodeSlab *
gum_exec_ctx_add_code_slab (GumExecCtx * ctx,
                            GumCodeSlab * code_slab)
{
  code_slab->slab.next = &ctx->code_slab->slab;
  ctx->code_slab = code_slab;
  return code_slab;
}

static GumDataSlab *
gum_exec_ctx_add_data_slab (GumExecCtx * ctx,
                            GumDataSlab * data_slab)
{
  data_slab->slab.next = &ctx->data_slab->slab;
  ctx->data_slab = data_slab;
  return data_slab;
}

static void
gum_exec_ctx_compute_code_address_spec (GumExecCtx * ctx,
                                        gsize slab_size,
                                        GumAddressSpec * spec)
{
  GumStalker * stalker = ctx->stalker;

  /* Code must be able to reference ExecCtx fields using 32-bit offsets. */
  spec->near_address = ctx;
  spec->max_distance = G_MAXINT32 - stalker->ctx_size - slab_size;
}

static void
gum_exec_ctx_compute_data_address_spec (GumExecCtx * ctx,
                                        gsize slab_size,
                                        GumAddressSpec * spec)
{
  GumStalker * stalker = ctx->stalker;

  /* Code must be able to reference ExecBlock fields using 32-bit offsets. */
  spec->near_address = ctx->code_slab;
  spec->max_distance = G_MAXINT32 - stalker->code_slab_size_dynamic - slab_size;
}

static gboolean
gum_exec_ctx_maybe_unfollow (GumExecCtx * ctx,
                             gpointer resume_at)
{
  if (g_atomic_int_get (&ctx->state) != GUM_EXEC_CTX_UNFOLLOW_PENDING)
    return FALSE;

  if (ctx->pending_calls > 0)
    return FALSE;

  gum_exec_ctx_unfollow (ctx, resume_at);

  return TRUE;
}

static void
gum_exec_ctx_unfollow (GumExecCtx * ctx,
                       gpointer resume_at)
{
  ctx->current_block = NULL;

  ctx->resume_at = resume_at;

  gum_tls_key_set_value (ctx->stalker->exec_ctx, NULL);

  ctx->destroy_pending_since = g_get_monotonic_time ();
  g_atomic_int_set (&ctx->state, GUM_EXEC_CTX_DESTROY_PENDING);
}

static gboolean
gum_exec_ctx_has_executed (GumExecCtx * ctx)
{
  return ctx->resume_at != NULL;
}

static gboolean
gum_exec_ctx_contains (GumExecCtx * ctx,
                       gconstpointer address)
{
  GumSlab * cur = &ctx->code_slab->slab;

  do {
    if ((const guint8 *) address >= cur->data &&
        (const guint8 *) address < (guint8 *) gum_slab_cursor (cur))
    {
      return TRUE;
    }

    cur = cur->next;
  } while (cur != NULL);

  return FALSE;
}

static gboolean
gum_exec_ctx_may_now_backpatch (GumExecCtx * ctx,
                                GumExecBlock * target_block)
{
  if (g_atomic_int_get (&ctx->state) != GUM_EXEC_CTX_ACTIVE)
    return FALSE;

  if ((target_block->flags & GUM_EXEC_BLOCK_ACTIVATION_TARGET) != 0)
    return FALSE;

  if (target_block->recycle_count < ctx->stalker->trust_threshold)
    return FALSE;

  return TRUE;
}

#define GUM_ENTRYGATE(name) \
    gum_exec_ctx_replace_current_block_from_##name
#define GUM_DEFINE_ENTRYGATE(name) \
    static gpointer GUM_THUNK \
    GUM_ENTRYGATE (name) ( \
        GumExecCtx * ctx, \
        gpointer start_address) \
    { \
      if (ctx->observer != NULL) \
        gum_stalker_observer_increment_##name (ctx->observer); \
      \
      return gum_exec_ctx_switch_block (ctx, start_address); \
    }

GUM_DEFINE_ENTRYGATE (call_imm)
GUM_DEFINE_ENTRYGATE (call_reg)
GUM_DEFINE_ENTRYGATE (call_mem)
GUM_DEFINE_ENTRYGATE (post_call_invoke)
GUM_DEFINE_ENTRYGATE (excluded_call_imm)
GUM_DEFINE_ENTRYGATE (ret_slow_path)

GUM_DEFINE_ENTRYGATE (jmp_imm)
GUM_DEFINE_ENTRYGATE (jmp_mem)
GUM_DEFINE_ENTRYGATE (jmp_reg)

GUM_DEFINE_ENTRYGATE (jmp_cond_imm)
GUM_DEFINE_ENTRYGATE (jmp_cond_mem)
GUM_DEFINE_ENTRYGATE (jmp_cond_reg)
GUM_DEFINE_ENTRYGATE (jmp_cond_jcxz)

GUM_DEFINE_ENTRYGATE (jmp_continuation)

#if GLIB_SIZEOF_VOID_P == 4 && !defined (HAVE_QNX)
GUM_DEFINE_ENTRYGATE (sysenter_slow_path)
#endif

static gpointer GUM_THUNK
gum_exec_ctx_switch_block (GumExecCtx * ctx,
                           gpointer start_address)
{
  if (ctx->observer != NULL)
    gum_stalker_observer_increment_total (ctx->observer);

  if (start_address == gum_stalker_unfollow_me ||
      start_address == gum_stalker_deactivate)
  {
    ctx->unfollow_called_while_still_following = TRUE;
    ctx->current_block = NULL;
    ctx->resume_at = start_address;
  }
  else if (start_address == _gum_thread_exit_impl)
  {
    gum_exec_ctx_unfollow (ctx, start_address);
  }
  else if (gum_exec_ctx_maybe_unfollow (ctx, start_address))
  {
  }
  else if (gum_exec_ctx_contains (ctx, start_address))
  {
    ctx->resume_at = start_address;
  }
  else
  {
    ctx->current_block = gum_exec_ctx_obtain_block_for (ctx, start_address,
        &ctx->resume_at);

    if (start_address == ctx->activation_target)
    {
      ctx->activation_target = NULL;
      ctx->current_block->flags |= GUM_EXEC_BLOCK_ACTIVATION_TARGET;
    }

    gum_exec_ctx_maybe_unfollow (ctx, start_address);
  }

  return ctx->resume_at;
}

static void
gum_exec_ctx_recompile_and_switch_block (GumExecCtx * ctx,
                                         gint32 * distance_to_data)
{
  GumExecBlock * block;
  gpointer start_address;

  block = (GumExecBlock *) ((guint8 *) distance_to_data + *distance_to_data);
  start_address = block->real_start;

  if (gum_exec_ctx_maybe_unfollow (ctx, start_address))
    return;

  gum_exec_ctx_recompile_block (ctx, block);

  ctx->current_block = block;
  ctx->resume_at = block->code_start;

  if (start_address == ctx->activation_target)
  {
    ctx->activation_target = NULL;
    ctx->current_block->flags |= GUM_EXEC_BLOCK_ACTIVATION_TARGET;
  }

  gum_exec_ctx_maybe_unfollow (ctx, start_address);
}

static GumExecBlock *
gum_exec_ctx_obtain_block_for (GumExecCtx * ctx,
                               gpointer real_address,
                               gpointer * code_address)
{
  GumExecBlock * block;

  gum_spinlock_acquire (&ctx->code_lock);

  block = gum_metal_hash_table_lookup (ctx->mappings, real_address);
  if (block != NULL)
  {
    const gint trust_threshold = ctx->stalker->trust_threshold;
    gboolean still_up_to_date;

    still_up_to_date =
        (trust_threshold >= 0 && block->recycle_count >= trust_threshold) ||
        memcmp (block->real_start, gum_exec_block_get_snapshot_start (block),
            block->real_size) == 0;

    gum_spinlock_release (&ctx->code_lock);

    if (still_up_to_date)
    {
      if (trust_threshold > 0)
        block->recycle_count++;
    }
    else
    {
      gum_exec_ctx_recompile_block (ctx, block);
    }
  }
  else
  {
    block = gum_exec_block_new (ctx);
    block->real_start = real_address;
    gum_exec_ctx_compile_block (ctx, block, real_address, block->code_start,
        GUM_ADDRESS (block->code_start), &block->real_size, &block->code_size);
    gum_exec_block_commit (block);

    gum_metal_hash_table_insert (ctx->mappings, real_address, block);

    gum_spinlock_release (&ctx->code_lock);

    gum_exec_ctx_maybe_emit_compile_event (ctx, block);
  }

  *code_address = block->code_start;

  return block;
}

static void
gum_exec_ctx_recompile_block (GumExecCtx * ctx,
                              GumExecBlock * block)
{
  GumStalker * stalker = ctx->stalker;
  guint8 * internal_code = block->code_start;
  GumCodeSlab * slab;
  guint8 * scratch_base;
  guint input_size, output_size;
  gsize new_snapshot_size, new_block_size;

  gum_spinlock_acquire (&ctx->code_lock);

  gum_stalker_thaw (stalker, internal_code, block->capacity);

  if (block->storage_block != NULL)
    gum_exec_block_clear (block->storage_block);
  gum_exec_block_clear (block);

  slab = block->code_slab;
  block->code_slab = ctx->scratch_slab;
  scratch_base = ctx->scratch_slab->slab.data;

  gum_exec_ctx_compile_block (ctx, block, block->real_start, scratch_base,
      GUM_ADDRESS (internal_code), &input_size, &output_size);

  block->code_slab = slab;

  new_snapshot_size =
      gum_stalker_snapshot_space_needed_for (stalker, input_size);

  new_block_size = output_size + new_snapshot_size;

  if (new_block_size <= block->capacity)
  {
    block->real_size = input_size;
    block->code_size = output_size;

    memcpy (internal_code, scratch_base, output_size);
    memcpy (gum_exec_block_get_snapshot_start (block), block->real_start,
        new_snapshot_size);

    gum_stalker_freeze (stalker, internal_code, new_block_size);
  }
  else
  {
    GumExecBlock * storage_block;
    GumX86Writer * cw = &ctx->code_writer;

    storage_block = gum_exec_block_new (ctx);
    storage_block->real_start = block->real_start;
    gum_exec_ctx_compile_block (ctx, block, block->real_start,
        storage_block->code_start, GUM_ADDRESS (storage_block->code_start),
        &storage_block->real_size, &storage_block->code_size);
    gum_exec_block_commit (storage_block);

    block->storage_block = storage_block;

    gum_stalker_thaw (stalker, internal_code, block->capacity);
    gum_x86_writer_reset (cw, internal_code);

    gum_x86_writer_put_jmp_address (cw,
        GUM_ADDRESS (storage_block->code_start));

    gum_x86_writer_flush (cw);
    gum_stalker_freeze (stalker, internal_code, block->capacity);
  }

  gum_spinlock_release (&ctx->code_lock);

  gum_exec_ctx_maybe_emit_compile_event (ctx, block);
}

static void
gum_exec_ctx_compile_block (GumExecCtx * ctx,
                            GumExecBlock * block,
                            gconstpointer input_code,
                            gpointer output_code,
                            GumAddress output_pc,
                            guint * input_size,
                            guint * output_size)
{
  GumX86Writer * cw = &ctx->code_writer;
  GumX86Relocator * rl = &ctx->relocator;
  GumGeneratorContext gc;
  GumStalkerIterator iterator;
  GumStalkerOutput output;
  gboolean all_labels_resolved;

  gum_x86_writer_reset (cw, output_code);
  cw->pc = output_pc;
  gum_x86_relocator_reset (rl, input_code, cw);

  gum_ensure_code_readable (input_code, ctx->stalker->page_size);

  gc.instruction = NULL;
  gc.relocator = rl;
  gc.code_writer = cw;
  gc.continuation_real_address = NULL;
  gc.opened_prolog = GUM_PROLOG_NONE;
  gc.accumulated_stack_delta = 0;

  iterator.exec_context = ctx;
  iterator.exec_block = block;
  iterator.generator_context = &gc;

  iterator.instruction.ci = NULL;
  iterator.instruction.start = NULL;
  iterator.instruction.end = NULL;
  iterator.requirements = GUM_REQUIRE_NOTHING;

  output.writer.x86 = cw;
  output.encoding = GUM_INSTRUCTION_DEFAULT;

  gum_exec_block_maybe_write_call_probe_code (block, &gc);

  ctx->pending_calls++;
  ctx->transform_block_impl (ctx->transformer, &iterator, &output);
  ctx->pending_calls--;

  if (gc.continuation_real_address != NULL)
  {
    GumBranchTarget continue_target = { 0, };

    continue_target.is_indirect = FALSE;
    continue_target.absolute_address = gc.continuation_real_address;

    gum_exec_block_write_jmp_transfer_code (block, &continue_target,
        GUM_ENTRYGATE (jmp_continuation), &gc);
  }

  gum_x86_writer_put_breakpoint (cw); /* Should never get here */

  all_labels_resolved = gum_x86_writer_flush (cw);
  if (!all_labels_resolved)
    g_error ("Failed to resolve labels");

  *input_size = rl->input_cur - rl->input_start;
  *output_size = gum_x86_writer_offset (cw);
}

static void
gum_exec_ctx_maybe_emit_compile_event (GumExecCtx * ctx,
                                       GumExecBlock * block)
{
  if ((ctx->sink_mask & GUM_COMPILE) != 0)
  {
    GumEvent ev;

    ev.type = GUM_COMPILE;
    ev.compile.start = block->real_start;
    ev.compile.end = block->real_start + block->real_size;

    ctx->sink_process_impl (ctx->sink, &ev, NULL);
  }
}

gboolean
gum_stalker_iterator_next (GumStalkerIterator * self,
                           const cs_insn ** insn)
{
  GumGeneratorContext * gc = self->generator_context;
  GumX86Relocator * rl = gc->relocator;
  GumInstruction * instruction;
  gboolean is_first_instruction;
  guint n_read;

  instruction = self->generator_context->instruction;
  is_first_instruction = instruction == NULL;

  if (instruction != NULL)
  {
    gboolean skip_implicitly_requested;

    skip_implicitly_requested = rl->outpos != rl->inpos;
    if (skip_implicitly_requested)
    {
      gum_x86_relocator_skip_one_no_label (rl);
    }

    if (gum_stalker_iterator_is_out_of_space (self))
    {
      gc->continuation_real_address = instruction->end;
      return FALSE;
    }
    else if (gum_x86_relocator_eob (rl))
    {
      return FALSE;
    }
  }

  instruction = &self->instruction;

  n_read = gum_x86_relocator_read_one (rl, &instruction->ci);
  if (n_read == 0)
    return FALSE;

  instruction->start = GSIZE_TO_POINTER (instruction->ci->address);
  instruction->end = instruction->start + instruction->ci->size;

  self->generator_context->instruction = instruction;

  if (is_first_instruction && (self->exec_context->sink_mask & GUM_BLOCK) != 0)
  {
    gum_exec_block_write_block_event_code (self->exec_block, gc,
        GUM_CODE_INTERRUPTIBLE);
  }

  if (insn != NULL)
    *insn = instruction->ci;

  return TRUE;
}

static gboolean
gum_stalker_iterator_is_out_of_space (GumStalkerIterator * self)
{
  GumExecBlock * block = self->exec_block;
  GumSlab * slab = &block->code_slab->slab;
  gsize capacity, snapshot_size;

  capacity = (guint8 *) gum_slab_end (slab) -
      (guint8 *) gum_x86_writer_cur (self->generator_context->code_writer);

  snapshot_size = gum_stalker_snapshot_space_needed_for (
      self->exec_context->stalker,
      self->generator_context->instruction->end - block->real_start);

  return capacity < GUM_EXEC_BLOCK_MIN_CAPACITY + snapshot_size +
      gum_stalker_get_ic_entry_size (self->exec_context->stalker);
}

static gsize
gum_stalker_get_ic_entry_size (GumStalker * self)
{
  return self->ic_entries * (2 * sizeof (gpointer));
}

void
gum_stalker_iterator_keep (GumStalkerIterator * self)
{
  GumExecBlock * block = self->exec_block;
  GumGeneratorContext * gc = self->generator_context;
  GumX86Relocator * rl = gc->relocator;
  const cs_insn * insn = gc->instruction->ci;
  GumVirtualizationRequirements requirements;

  if ((self->exec_context->sink_mask & GUM_EXEC) != 0)
    gum_exec_block_write_exec_event_code (block, gc, GUM_CODE_INTERRUPTIBLE);

  switch (insn->id)
  {
    case X86_INS_CALL:
    case X86_INS_JMP:
      requirements = gum_exec_block_virtualize_branch_insn (block, gc);
      break;
    case X86_INS_RET:
      requirements = gum_exec_block_virtualize_ret_insn (block, gc);
      break;
    case X86_INS_SYSENTER:
      requirements = gum_exec_block_virtualize_sysenter_insn (block, gc);
      break;
    case X86_INS_JECXZ:
    case X86_INS_JRCXZ:
      requirements = gum_exec_block_virtualize_branch_insn (block, gc);
      break;
    default:
      if (gum_x86_reader_insn_is_jcc (insn))
        requirements = gum_exec_block_virtualize_branch_insn (block, gc);
      else
        requirements = GUM_REQUIRE_RELOCATION;
      break;
  }

  gum_exec_block_close_prolog (block, gc);

  if ((requirements & GUM_REQUIRE_RELOCATION) != 0)
  {
    gum_x86_relocator_write_one_no_label (rl);
  }
  else if ((requirements & GUM_REQUIRE_SINGLE_STEP) != 0)
  {
    gum_x86_relocator_skip_one_no_label (rl);
    gum_exec_block_write_single_step_transfer_code (block, gc);
  }

  self->requirements = requirements;
}

static void
gum_exec_ctx_emit_call_event (GumExecCtx * ctx,
                              gpointer location,
                              gpointer target,
                              GumCpuContext * cpu_context)
{
  GumEvent ev;
  GumCallEvent * call = &ev.call;

  ev.type = GUM_CALL;

  call->location = location;
  call->target = target;
  call->depth = ctx->first_frame - ctx->current_frame;

  GUM_CPU_CONTEXT_XIP (cpu_context) = GPOINTER_TO_SIZE (location);

  ctx->sink_process_impl (ctx->sink, &ev, cpu_context);
}

static void
gum_exec_ctx_emit_ret_event (GumExecCtx * ctx,
                             gpointer location,
                             GumCpuContext * cpu_context)
{
  GumEvent ev;
  GumRetEvent * ret = &ev.ret;

  ev.type = GUM_RET;

  ret->location = location;
  ret->target = *((gpointer *) ctx->app_stack);
  ret->depth = ctx->first_frame - ctx->current_frame;

  GUM_CPU_CONTEXT_XIP (cpu_context) = GPOINTER_TO_SIZE (location);

  ctx->sink_process_impl (ctx->sink, &ev, cpu_context);
}

static void
gum_exec_ctx_emit_exec_event (GumExecCtx * ctx,
                              gpointer location,
                              GumCpuContext * cpu_context)
{
  GumEvent ev;
  GumExecEvent * exec = &ev.exec;

  ev.type = GUM_EXEC;

  exec->location = location;

  GUM_CPU_CONTEXT_XIP (cpu_context) = GPOINTER_TO_SIZE (location);

  ctx->sink_process_impl (ctx->sink, &ev, cpu_context);
}

static void
gum_exec_ctx_emit_block_event (GumExecCtx * ctx,
                               const GumExecBlock * block,
                               GumCpuContext * cpu_context)
{
  GumEvent ev;
  GumBlockEvent * bev = &ev.block;

  ev.type = GUM_BLOCK;

  bev->start = block->real_start;
  bev->end = block->real_start + block->real_size;

  GUM_CPU_CONTEXT_XIP (cpu_context) = GPOINTER_TO_SIZE (block->real_start);

  ctx->sink_process_impl (ctx->sink, &ev, cpu_context);
}

void
gum_stalker_iterator_put_callout (GumStalkerIterator * self,
                                  GumStalkerCallout callout,
                                  gpointer data,
                                  GDestroyNotify data_destroy)
{
  GumExecBlock * block = self->exec_block;
  GumGeneratorContext * gc = self->generator_context;
  GumX86Writer * cw = gc->code_writer;
  GumCalloutEntry entry;
  GumAddress entry_address;

  entry.callout = callout;
  entry.data = data;
  entry.data_destroy = data_destroy;
  entry.pc = gc->instruction->start;
  entry.exec_context = self->exec_context;
  entry.next = gum_exec_block_get_last_callout_entry (block);
  gum_exec_block_write_inline_data (cw, &entry, sizeof (entry), &entry_address);

  gum_exec_block_set_last_callout_entry (block,
      GSIZE_TO_POINTER (entry_address));

  gum_exec_block_open_prolog (block, GUM_PROLOG_FULL, gc);
  gum_x86_writer_put_call_address_with_aligned_arguments (cw,
      GUM_CALL_CAPI, GUM_ADDRESS (gum_stalker_invoke_callout), 2,
      GUM_ARG_ADDRESS, entry_address,
      GUM_ARG_REGISTER, GUM_REG_XBX);
  gum_exec_block_close_prolog (block, gc);
}

static void
gum_stalker_invoke_callout (GumCalloutEntry * entry,
                            GumCpuContext * cpu_context)
{
  GumExecCtx * ec = entry->exec_context;

  GUM_CPU_CONTEXT_XIP (cpu_context) = GPOINTER_TO_SIZE (entry->pc);

  ec->pending_calls++;
  entry->callout (cpu_context, entry->data);
  ec->pending_calls--;
}

static void
gum_exec_ctx_write_prolog (GumExecCtx * ctx,
                           GumPrologType type,
                           GumX86Writer * cw)
{
  switch (type)
  {
    case GUM_PROLOG_MINIMAL:
    case GUM_PROLOG_FULL:
    {
      gpointer helper;

      helper = (type == GUM_PROLOG_MINIMAL)
          ? ctx->last_prolog_minimal
          : ctx->last_prolog_full;

      gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP,
          GUM_REG_XSP, -GUM_RED_ZONE_SIZE);
      gum_x86_writer_put_call_address (cw, GUM_ADDRESS (helper));

      break;
    }
    case GUM_PROLOG_IC:
    {
      gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP,
          GUM_REG_XSP, -GUM_RED_ZONE_SIZE);
      gum_x86_writer_put_pushfx (cw);
      gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
      gum_x86_writer_put_push_reg (cw, GUM_REG_XBX);
      gum_x86_writer_put_mov_reg_reg (cw, GUM_REG_XBX, GUM_REG_XSP);

      gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XAX, GUM_REG_XSP,
          3 * sizeof (gpointer) + GUM_RED_ZONE_SIZE);
      gum_x86_writer_put_mov_near_ptr_reg (cw, GUM_ADDRESS (&ctx->app_stack),
          GUM_REG_XAX);

      break;
    }
    default:
    {
      g_assert_not_reached ();
      break;
    }
  }
}

static void
gum_exec_ctx_write_epilog (GumExecCtx * ctx,
                           GumPrologType type,
                           GumX86Writer * cw)
{
  switch (type)
  {
    case GUM_PROLOG_MINIMAL:
    case GUM_PROLOG_FULL:
    {
      gpointer helper;

      helper = (type == GUM_PROLOG_MINIMAL)
          ? ctx->last_epilog_minimal
          : ctx->last_epilog_full;

      gum_x86_writer_put_call_address (cw, GUM_ADDRESS (helper));
      gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XSP,
          GUM_ADDRESS (&ctx->app_stack));

      break;
    }
    case GUM_PROLOG_IC:
    {
      gum_x86_writer_put_pop_reg (cw, GUM_REG_XBX);
      gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
      gum_x86_writer_put_popfx (cw);
      gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XSP,
          GUM_ADDRESS (&ctx->app_stack));

      break;
    }
    default:
    {
      g_assert_not_reached ();
      break;
    }
  }
}

static void
gum_exec_ctx_ensure_inline_helpers_reachable (GumExecCtx * ctx)
{
  gum_exec_ctx_ensure_helper_reachable (ctx, &ctx->last_prolog_minimal,
      gum_exec_ctx_write_minimal_prolog_helper);
  gum_exec_ctx_ensure_helper_reachable (ctx, &ctx->last_epilog_minimal,
      gum_exec_ctx_write_minimal_epilog_helper);

  gum_exec_ctx_ensure_helper_reachable (ctx, &ctx->last_prolog_full,
      gum_exec_ctx_write_full_prolog_helper);
  gum_exec_ctx_ensure_helper_reachable (ctx, &ctx->last_epilog_full,
      gum_exec_ctx_write_full_epilog_helper);

  gum_exec_ctx_ensure_helper_reachable (ctx, &ctx->last_stack_push,
      gum_exec_ctx_write_stack_push_helper);
  gum_exec_ctx_ensure_helper_reachable (ctx, &ctx->last_stack_pop_and_go,
      gum_exec_ctx_write_stack_pop_and_go_helper);

  gum_exec_ctx_ensure_helper_reachable (ctx, &ctx->last_invalidator,
      gum_exec_ctx_write_invalidator);
}

static void
gum_exec_ctx_write_minimal_prolog_helper (GumExecCtx * ctx,
                                          GumX86Writer * cw)
{
  gum_exec_ctx_write_prolog_helper (ctx, GUM_PROLOG_MINIMAL, cw);
}

static void
gum_exec_ctx_write_minimal_epilog_helper (GumExecCtx * ctx,
                                          GumX86Writer * cw)
{
  gum_exec_ctx_write_epilog_helper (ctx, GUM_PROLOG_MINIMAL, cw);
}

static void
gum_exec_ctx_write_full_prolog_helper (GumExecCtx * ctx,
                                       GumX86Writer * cw)
{
  gum_exec_ctx_write_prolog_helper (ctx, GUM_PROLOG_FULL, cw);
}

static void
gum_exec_ctx_write_full_epilog_helper (GumExecCtx * ctx,
                                       GumX86Writer * cw)
{
  gum_exec_ctx_write_epilog_helper (ctx, GUM_PROLOG_FULL, cw);
}

static void
gum_exec_ctx_write_prolog_helper (GumExecCtx * ctx,
                                  GumPrologType type,
                                  GumX86Writer * cw)
{
  guint8 fxsave[] = {
    0x0f, 0xae, 0x04, 0x24 /* fxsave [esp] */
  };
  guint8 upper_ymm_saver[] = {
#if GLIB_SIZEOF_VOID_P == 8
    /* vextracti128 ymm0..ymm15, [rsp+0x0]..[rsp+0xF0], 1 */
    0xc4, 0xe3, 0x7d, 0x39, 0x04, 0x24, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x4c, 0x24, 0x10, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x54, 0x24, 0x20, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x5c, 0x24, 0x30, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x64, 0x24, 0x40, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x6c, 0x24, 0x50, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x74, 0x24, 0x60, 0x01,
    0xc4, 0xe3, 0x7d, 0x39, 0x7c, 0x24, 0x70, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0x8c, 0x24, 0x90, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0x94, 0x24, 0xa0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0x9c, 0x24, 0xb0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0xa4, 0x24, 0xc0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0xac, 0x24, 0xd0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0xb4, 0x24, 0xe0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x7d, 0x39, 0xbc, 0x24, 0xf0, 0x00, 0x00, 0x00, 0x01
#else
    /* vextracti128 ymm0..ymm7, [esp+0x0]..[esp+0x70], 1 */
    0xc4, 0xc3, 0x7d, 0x39, 0x04, 0x24, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x4c, 0x24, 0x10, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x54, 0x24, 0x20, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x5c, 0x24, 0x30, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x64, 0x24, 0x40, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x6c, 0x24, 0x50, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x74, 0x24, 0x60, 0x01,
    0xc4, 0xc3, 0x7d, 0x39, 0x7c, 0x24, 0x70, 0x01
#endif
  };

  gum_x86_writer_put_pushfx (cw);
  gum_x86_writer_put_cld (cw); /* C ABI mandates this */

  if (type == GUM_PROLOG_MINIMAL)
  {
    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);

    gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XAX, GUM_REG_XSP,
        3 * sizeof (gpointer) + GUM_RED_ZONE_SIZE);
    gum_x86_writer_put_mov_near_ptr_reg (cw, GUM_ADDRESS (&ctx->app_stack),
        GUM_REG_XAX);

    gum_x86_writer_put_push_reg (cw, GUM_REG_XCX);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XDX);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XBX);

#if GLIB_SIZEOF_VOID_P == 8
    gum_x86_writer_put_push_reg (cw, GUM_REG_XSI);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XDI);
    gum_x86_writer_put_push_reg (cw, GUM_REG_R8);
    gum_x86_writer_put_push_reg (cw, GUM_REG_R9);
    gum_x86_writer_put_push_reg (cw, GUM_REG_R10);
    gum_x86_writer_put_push_reg (cw, GUM_REG_R11);
#endif
  }
  else /* GUM_PROLOG_FULL */
  {
    gum_x86_writer_put_pushax (cw); /* All of GumCpuContext except for xip */
    /* GumCpuContext.xip gets filled out later */
    gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP, GUM_REG_XSP,
        -((gint) sizeof (gpointer)));

    gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XAX, GUM_REG_XSP,
        sizeof (GumCpuContext) + 2 * sizeof (gpointer) + GUM_RED_ZONE_SIZE);
    gum_x86_writer_put_mov_near_ptr_reg (cw, GUM_ADDRESS (&ctx->app_stack),
        GUM_REG_XAX);

    gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
        GUM_REG_XSP, GUM_CPU_CONTEXT_OFFSET_XSP,
        GUM_REG_XAX);
  }

  gum_x86_writer_put_mov_reg_reg (cw, GUM_REG_XBX, GUM_REG_XSP);
  gum_x86_writer_put_and_reg_u32 (cw, GUM_REG_XSP, (guint32) ~(16 - 1));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP, 512);
  gum_x86_writer_put_bytes (cw, fxsave, sizeof (fxsave));

  if ((ctx->stalker->cpu_features & GUM_CPU_AVX2) != 0)
  {
    gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP, 0x100);
    gum_x86_writer_put_bytes (cw, upper_ymm_saver, sizeof (upper_ymm_saver));
  }

  /* Jump to our caller but leave it on the stack */
  gum_x86_writer_put_jmp_reg_offset_ptr (cw,
      GUM_REG_XBX, (type == GUM_PROLOG_MINIMAL)
          ? GUM_MINIMAL_PROLOG_RETURN_OFFSET
          : GUM_FULL_PROLOG_RETURN_OFFSET);
}

static void
gum_exec_ctx_write_epilog_helper (GumExecCtx * ctx,
                                  GumPrologType type,
                                  GumX86Writer * cw)
{
  guint8 fxrstor[] = {
    0x0f, 0xae, 0x0c, 0x24 /* fxrstor [esp] */
  };
  guint8 upper_ymm_restorer[] = {
#if GLIB_SIZEOF_VOID_P == 8
    /* vinserti128 ymm0..ymm15, ymm0..ymm15, [rsp+0x0]..[rsp+0xF0], 1 */
    0xc4, 0xe3, 0x7d, 0x38, 0x04, 0x24, 0x01,
    0xc4, 0xe3, 0x75, 0x38, 0x4c, 0x24, 0x10, 0x01,
    0xc4, 0xe3, 0x6d, 0x38, 0x54, 0x24, 0x20, 0x01,
    0xc4, 0xe3, 0x65, 0x38, 0x5c, 0x24, 0x30, 0x01,
    0xc4, 0xe3, 0x5d, 0x38, 0x64, 0x24, 0x40, 0x01,
    0xc4, 0xe3, 0x55, 0x38, 0x6c, 0x24, 0x50, 0x01,
    0xc4, 0xe3, 0x4d, 0x38, 0x74, 0x24, 0x60, 0x01,
    0xc4, 0xe3, 0x45, 0x38, 0x7c, 0x24, 0x70, 0x01,
    0xc4, 0x63, 0x3d, 0x38, 0x84, 0x24, 0x80, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x35, 0x38, 0x8c, 0x24, 0x90, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x2d, 0x38, 0x94, 0x24, 0xa0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x25, 0x38, 0x9c, 0x24, 0xb0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x1d, 0x38, 0xa4, 0x24, 0xc0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x15, 0x38, 0xac, 0x24, 0xd0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x0d, 0x38, 0xb4, 0x24, 0xe0, 0x00, 0x00, 0x00, 0x01,
    0xc4, 0x63, 0x05, 0x38, 0xbc, 0x24, 0xf0, 0x00, 0x00, 0x00, 0x01
#else
    /* vinserti128 ymm0..ymm7, ymm0..ymm7, [esp+0x0]..[esp+0x70], 1 */
    0xc4, 0xc3, 0x7d, 0x38, 0x04, 0x24, 0x01,
    0xc4, 0xc3, 0x75, 0x38, 0x4c, 0x24, 0x10, 0x01,
    0xc4, 0xc3, 0x6d, 0x38, 0x54, 0x24, 0x20, 0x01,
    0xc4, 0xc3, 0x65, 0x38, 0x5c, 0x24, 0x30, 0x01,
    0xc4, 0xc3, 0x5d, 0x38, 0x64, 0x24, 0x40, 0x01,
    0xc4, 0xc3, 0x55, 0x38, 0x6c, 0x24, 0x50, 0x01,
    0xc4, 0xc3, 0x4d, 0x38, 0x74, 0x24, 0x60, 0x01,
    0xc4, 0xc3, 0x45, 0x38, 0x7c, 0x24, 0x70, 0x01
#endif
  };

  /* Store our caller in the return address created by the prolog */
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XBX, (type == GUM_PROLOG_MINIMAL)
          ? GUM_MINIMAL_PROLOG_RETURN_OFFSET
          : GUM_FULL_PROLOG_RETURN_OFFSET,
      GUM_REG_XAX);

  if ((ctx->stalker->cpu_features & GUM_CPU_AVX2) != 0)
  {
    gum_x86_writer_put_bytes (cw, upper_ymm_restorer,
        sizeof (upper_ymm_restorer));
    gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP, 0x100);
  }

  gum_x86_writer_put_bytes (cw, fxrstor, sizeof (fxrstor));
  gum_x86_writer_put_mov_reg_reg (cw, GUM_REG_XSP, GUM_REG_XBX);

  if (type == GUM_PROLOG_MINIMAL)
  {
#if GLIB_SIZEOF_VOID_P == 8
    gum_x86_writer_put_pop_reg (cw, GUM_REG_R11);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_R10);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_R9);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_R8);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XDI);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XSI);
#endif

    gum_x86_writer_put_pop_reg (cw, GUM_REG_XBX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  }
  else /* GUM_PROLOG_FULL */
  {
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX); /* Discard
                                                     GumCpuContext.xip */
    gum_x86_writer_put_popax (cw);
  }

  gum_x86_writer_put_popfx (cw);
  gum_x86_writer_put_ret (cw);
}

static void
gum_exec_ctx_write_stack_push_helper (GumExecCtx * ctx,
                                      GumX86Writer * cw)
{
  gconstpointer skip_stack_push = cw->code + 1;

  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (&ctx->current_frame));
  gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);

  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XAX);
  gum_x86_writer_put_test_reg_u32 (cw, GUM_REG_XAX,
      ctx->stalker->page_size - 1);
  gum_x86_writer_put_jcc_short_label (cw, X86_INS_JE, skip_stack_push,
      GUM_UNLIKELY);

  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XAX, sizeof (GumExecFrame));

  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XAX, GUM_REG_XCX);
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumExecFrame, code_address), GUM_REG_XDX);

  gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XCX, GUM_REG_XAX);
  gum_x86_writer_put_ret (cw);

  gum_x86_writer_put_label (cw, skip_stack_push);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_ret (cw);
}

static void
gum_exec_ctx_write_stack_pop_and_go_helper (GumExecCtx * ctx,
                                            GumX86Writer * cw)
{
  gconstpointer resolve_dynamically = cw->code + 1;
  gconstpointer check_slab = cw->code + 2;
  gconstpointer next_slab = cw->code + 3;
  GumAddress return_at = GUM_ADDRESS (&ctx->return_at);
  guint stack_delta = GUM_RED_ZONE_SIZE + sizeof (gpointer);

  /*
   * Fast path (try the stack)
   */
  gum_x86_writer_put_pushfx (cw);
  gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
  stack_delta += 2 * sizeof (gpointer);

  /*
   * We want to jump to the origin ret instruction after modifying the
   * return address on the stack.
   */
  gum_x86_writer_put_mov_near_ptr_reg (cw, return_at, GUM_REG_XCX);

  /* Check frame at the top of the stack */
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (&ctx->current_frame));
  gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
  stack_delta += sizeof (gpointer);
  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XAX);

  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XCX, GUM_REG_XAX);
  gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw,
      GUM_REG_XSP, stack_delta,
      GUM_REG_XCX);
  gum_x86_writer_put_jcc_short_label (cw, X86_INS_JNE,
      resolve_dynamically, GUM_UNLIKELY);

  /* Replace return address */
  gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, GUM_REG_XCX,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumExecFrame, code_address));
  gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
      GUM_REG_XSP, stack_delta,
      GUM_REG_XCX);

  /* Pop from our stack */
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XAX, sizeof (GumExecFrame));
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XCX, GUM_REG_XAX);

  /* Proceeed to block */
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_popfx (cw);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP,
      GUM_REG_XSP, GUM_RED_ZONE_SIZE);

  gum_x86_writer_put_jmp_near_ptr (cw, return_at);

  gum_x86_writer_put_label (cw, resolve_dynamically);

  /* Clear our stack so we might resync later */
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (ctx->first_frame));
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XAX, GUM_REG_XCX);

  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_popfx (cw);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP,
      GUM_REG_XSP, GUM_RED_ZONE_SIZE);

  /*
   * Check if the target is already in one of the slabs.
   */
  gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_push_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_push_reg (cw, GUM_REG_XDX);

  /*
   * Our stack is clear here except for the 3 registers we just saved above,
   * the stack_delta therefore is the offset of the return address from XSP.
   */
  stack_delta = sizeof (gpointer) * 3;

  /* GumSlab * cur(XAX) = &ctx->code_slab->slab; */
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX, GUM_ADDRESS (ctx));
  gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, GUM_REG_XAX,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumExecCtx, code_slab));

  if (G_STRUCT_OFFSET (GumCodeSlab, slab) != 0)
  {
    gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XAX,
        G_STRUCT_OFFSET (GumCodeSlab, slab));
  }

  /* do */
  gum_x86_writer_put_label (cw, check_slab);

  /* data(XCX) = curr->data */
  gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, GUM_REG_XCX,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumSlab, data));

  /* IF return_address < data THEN continue */
  gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw, GUM_REG_XSP, stack_delta,
      GUM_REG_XCX);
  gum_x86_writer_put_jcc_short_label (cw, X86_INS_JLE, next_slab, GUM_LIKELY);

  /* offset(XDX) = curr->offset */
  gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, GUM_REG_EDX,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumSlab, offset));

  /* limit(XCX) = data + offset */
  gum_x86_writer_put_add_reg_reg (cw, GUM_REG_XCX, GUM_REG_XDX);

  /* IF return_address > limit THEN continue */
  gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw, GUM_REG_XSP, stack_delta,
      GUM_REG_XCX);
  gum_x86_writer_put_jcc_short_label (cw, X86_INS_JGE, next_slab, GUM_LIKELY);

  /* Our target is within a slab, we can just unwind. */
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_jmp_near_ptr (cw, return_at);

  gum_x86_writer_put_label (cw, next_slab);

  /* cur = cur->next; */
  gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, GUM_REG_XAX,
      GUM_REG_XAX, G_STRUCT_OFFSET (GumSlab, next));

  /* while (cur != NULL); */
  gum_x86_writer_put_test_reg_reg (cw, GUM_REG_XAX, GUM_REG_XAX);
  gum_x86_writer_put_jcc_short_label (cw, X86_INS_JNE, check_slab, GUM_LIKELY);

  gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);

  /*
   * Slow path (resolve dynamically)
   */
  gum_exec_ctx_write_prolog (ctx, GUM_PROLOG_MINIMAL, cw);

  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (&ctx->app_stack));
  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XAX);
  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_THUNK_REG_ARG1, GUM_REG_XAX);
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);

  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (GUM_ENTRYGATE (ret_slow_path)));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);

  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (&ctx->app_stack));
  gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XCX, GUM_REG_XCX);
  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XCX, GUM_REG_XAX);

  gum_exec_ctx_write_epilog (ctx, GUM_PROLOG_MINIMAL, cw);

  gum_x86_writer_put_jmp_near_ptr (cw, return_at);
}

static void
gum_exec_ctx_write_invalidator (GumExecCtx * ctx,
                                GumX86Writer * cw)
{
  /* Swap XDI and the top-of-stack return address */
  gum_x86_writer_put_xchg_reg_reg_ptr (cw, GUM_REG_XDI, GUM_REG_XSP);

  gum_exec_ctx_write_prolog (ctx, GUM_PROLOG_MINIMAL, cw);

  gum_x86_writer_put_call_address_with_aligned_arguments (cw,
      GUM_CALL_CAPI, GUM_ADDRESS (gum_exec_ctx_recompile_and_switch_block), 2,
      GUM_ARG_ADDRESS, GUM_ADDRESS (ctx),
      GUM_ARG_REGISTER, GUM_REG_XDI);

  gum_exec_ctx_write_epilog (ctx, GUM_PROLOG_MINIMAL, cw);

  gum_x86_writer_put_pop_reg (cw, GUM_REG_XDI);
  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP, GUM_REG_XSP,
      GUM_RED_ZONE_SIZE);

  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&ctx->resume_at));
}

static void
gum_exec_ctx_ensure_helper_reachable (GumExecCtx * ctx,
                                      gpointer * helper_ptr,
                                      GumExecHelperWriteFunc write)
{
  GumSlab * slab = &ctx->code_slab->slab;
  GumX86Writer * cw = &ctx->code_writer;
  gpointer start;

  if (gum_exec_ctx_is_helper_reachable (ctx, helper_ptr))
    return;

  start = gum_slab_cursor (slab);
  gum_stalker_thaw (ctx->stalker, start, gum_slab_available (slab));
  gum_x86_writer_reset (cw, start);
  *helper_ptr = gum_x86_writer_cur (cw);

  write (ctx, cw);

  gum_x86_writer_flush (cw);
  gum_stalker_freeze (ctx->stalker, cw->base, gum_x86_writer_offset (cw));

  gum_slab_reserve (slab, gum_x86_writer_offset (cw));
}

static gboolean
gum_exec_ctx_is_helper_reachable (GumExecCtx * ctx,
                                  gpointer * helper_ptr)
{
  GumSlab * slab = &ctx->code_slab->slab;
  GumAddress helper, start, end;

  helper = GUM_ADDRESS (*helper_ptr);
  if (helper == 0)
    return FALSE;

  start = GUM_ADDRESS (gum_slab_start (slab));
  end = GUM_ADDRESS (gum_slab_end (slab));

  if (!gum_x86_writer_can_branch_directly_between (start, helper))
    return FALSE;

  return gum_x86_writer_can_branch_directly_between (end, helper);
}

static void
gum_exec_ctx_write_push_branch_target_address (GumExecCtx * ctx,
                                               const GumBranchTarget * target,
                                               GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;

  if (!target->is_indirect)
  {
    if (target->base == X86_REG_INVALID)
    {
      gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
      gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
          GUM_ADDRESS (target->absolute_address));
      gum_x86_writer_put_xchg_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XSP);
    }
    else
    {
      gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
      gum_exec_ctx_load_real_register_into (ctx, GUM_REG_XAX,
          gum_cpu_reg_from_capstone (target->base),
          target->origin_ip,
          gc);
      gum_x86_writer_put_xchg_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XSP);
    }
  }
  else if (target->base == X86_REG_INVALID && target->index == X86_REG_INVALID)
  {
    g_assert (target->scale == 1);
    g_assert (target->absolute_address != NULL);
    g_assert (target->relative_offset == 0);

#if GLIB_SIZEOF_VOID_P == 8
    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
    gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
        GUM_ADDRESS (target->absolute_address));
    gum_write_segment_prefix (target->pfx_seg, cw);
    gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_RAX, GUM_REG_RAX);
    gum_x86_writer_put_xchg_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XSP);
#else
    gum_write_segment_prefix (target->pfx_seg, cw);
    gum_x86_writer_put_u8 (cw, 0xff);
    gum_x86_writer_put_u8 (cw, 0x35);
    gum_x86_writer_put_bytes (cw, (guint8 *) &target->absolute_address,
        sizeof (target->absolute_address));
#endif
  }
  else
  {
    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX); /* Placeholder */

    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XDX);

    gum_exec_ctx_load_real_register_into (ctx, GUM_REG_XAX,
        gum_cpu_reg_from_capstone (target->base),
        target->origin_ip,
        gc);
    gum_exec_ctx_load_real_register_into (ctx, GUM_REG_XDX,
        gum_cpu_reg_from_capstone (target->index),
        target->origin_ip,
        gc);
    gum_x86_writer_put_mov_reg_base_index_scale_offset_ptr (cw, GUM_REG_XAX,
        GUM_REG_XAX, GUM_REG_XDX, target->scale,
        target->relative_offset);
    gum_x86_writer_put_mov_reg_offset_ptr_reg (cw,
        GUM_REG_XSP, 2 * sizeof (gpointer),
        GUM_REG_XAX);

    gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
  }
}

static void
gum_exec_ctx_load_real_register_into (GumExecCtx * ctx,
                                      GumCpuReg target_register,
                                      GumCpuReg source_register,
                                      gpointer ip,
                                      GumGeneratorContext * gc)
{
  switch (gc->opened_prolog)
  {
    case GUM_PROLOG_MINIMAL:
      gum_exec_ctx_load_real_register_from_minimal_frame_into (ctx,
          target_register, source_register, ip, gc);
      break;
    case GUM_PROLOG_FULL:
      gum_exec_ctx_load_real_register_from_full_frame_into (ctx,
          target_register, source_register, ip, gc);
      break;
    case GUM_PROLOG_IC:
      gum_exec_ctx_load_real_register_from_ic_frame_into (ctx, target_register,
          source_register, ip, gc);
      break;
    default:
      g_assert_not_reached ();
      break;
  }
}

static void
gum_exec_ctx_load_real_register_from_minimal_frame_into (
    GumExecCtx * ctx,
    GumCpuReg target_register,
    GumCpuReg source_register,
    gpointer ip,
    GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  GumCpuReg source_meta;

  source_meta = gum_cpu_meta_reg_from_real_reg (source_register);

  if (source_meta >= GUM_REG_XAX && source_meta <= GUM_REG_XBX)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX,
        GUM_STATE_PRESERVE_TOPMOST_REGISTER_INDEX * sizeof (gpointer) -
        ((source_meta - GUM_REG_XAX) * sizeof (gpointer)));
  }
#if GLIB_SIZEOF_VOID_P == 8
  else if (source_meta >= GUM_REG_XSI && source_meta <= GUM_REG_XDI)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX,
        GUM_STATE_PRESERVE_TOPMOST_REGISTER_INDEX * sizeof (gpointer) -
        ((source_meta - 2 - GUM_REG_XAX) * sizeof (gpointer)));
  }
  else if (source_meta >= GUM_REG_R8 && source_meta <= GUM_REG_R11)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX,
        GUM_STATE_PRESERVE_TOPMOST_REGISTER_INDEX * sizeof (gpointer) -
        ((source_meta - 2 - GUM_REG_RAX) * sizeof (gpointer)));
  }
#endif
  else if (source_meta == GUM_REG_XSP)
  {
    gum_x86_writer_put_mov_reg_near_ptr (cw, target_register,
        GUM_ADDRESS (&ctx->app_stack));
    gum_x86_writer_put_lea_reg_reg_offset (cw, target_register,
        target_register, gc->accumulated_stack_delta);
  }
  else if (source_meta == GUM_REG_XIP)
  {
    gum_x86_writer_put_mov_reg_address (cw, target_register, GUM_ADDRESS (ip));
  }
  else if (source_meta == GUM_REG_NONE)
  {
    gum_x86_writer_put_xor_reg_reg (cw, target_register, target_register);
  }
  else
  {
    gum_x86_writer_put_mov_reg_reg (cw, target_register, source_register);
  }
}

static void
gum_exec_ctx_load_real_register_from_full_frame_into (GumExecCtx * ctx,
                                                      GumCpuReg target_register,
                                                      GumCpuReg source_register,
                                                      gpointer ip,
                                                      GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  GumCpuReg source_meta;

  source_meta = gum_cpu_meta_reg_from_real_reg (source_register);

  if (source_meta >= GUM_REG_XAX && source_meta <= GUM_REG_XBX)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX, sizeof (GumCpuContext) -
        ((source_meta - GUM_REG_XAX + 1) * sizeof (gpointer)));
  }
  else if (source_meta >= GUM_REG_XBP && source_meta <= GUM_REG_XDI)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX, sizeof (GumCpuContext) -
        ((source_meta - GUM_REG_XAX + 1) * sizeof (gpointer)));
  }
#if GLIB_SIZEOF_VOID_P == 8
  else if (source_meta >= GUM_REG_R8 && source_meta <= GUM_REG_R15)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register,
        GUM_REG_XBX, sizeof (GumCpuContext) -
        ((source_meta - GUM_REG_RAX + 1) * sizeof (gpointer)));
  }
#endif
  else if (source_meta == GUM_REG_XSP)
  {
    gum_x86_writer_put_mov_reg_near_ptr (cw, target_register,
        GUM_ADDRESS (&ctx->app_stack));
    gum_x86_writer_put_lea_reg_reg_offset (cw, target_register,
        target_register, gc->accumulated_stack_delta);
  }
  else if (source_meta == GUM_REG_XIP)
  {
    gum_x86_writer_put_mov_reg_address (cw, target_register, GUM_ADDRESS (ip));
  }
  else if (source_meta == GUM_REG_NONE)
  {
    gum_x86_writer_put_xor_reg_reg (cw, target_register, target_register);
  }
  else
  {
    gum_x86_writer_put_mov_reg_reg (cw, target_register, source_register);
  }
}

static void
gum_exec_ctx_load_real_register_from_ic_frame_into (GumExecCtx * ctx,
                                                    GumCpuReg target_register,
                                                    GumCpuReg source_register,
                                                    gpointer ip,
                                                    GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;
  GumCpuReg source_meta;

  source_meta = gum_cpu_meta_reg_from_real_reg (source_register);

  if (source_meta == GUM_REG_XAX)
  {
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, target_register, GUM_REG_XBX,
        sizeof (gpointer));
  }
  else if (source_meta == GUM_REG_XBX)
  {
    gum_x86_writer_put_mov_reg_reg_ptr (cw, target_register, GUM_REG_XBX);
  }
  else if (source_meta == GUM_REG_XSP)
  {
    gum_x86_writer_put_mov_reg_near_ptr (cw, target_register,
        GUM_ADDRESS (&ctx->app_stack));
    gum_x86_writer_put_lea_reg_reg_offset (cw, target_register,
        target_register, gc->accumulated_stack_delta);
  }
  else if (source_meta == GUM_REG_XIP)
  {
    gum_x86_writer_put_mov_reg_address (cw, target_register, GUM_ADDRESS (ip));
  }
  else if (source_meta == GUM_REG_NONE)
  {
    gum_x86_writer_put_xor_reg_reg (cw, target_register, target_register);
  }
  else
  {
    gum_x86_writer_put_mov_reg_reg (cw, target_register, source_register);
  }
}

static GumExecBlock *
gum_exec_block_new (GumExecCtx * ctx)
{
  GumExecBlock * block;
  GumStalker * stalker = ctx->stalker;
  GumCodeSlab * code_slab = ctx->code_slab;
  GumDataSlab * data_slab = ctx->data_slab;
  gsize code_available;

  code_available = gum_slab_available (&code_slab->slab);
  if (code_available < GUM_EXEC_BLOCK_MIN_CAPACITY +
      gum_stalker_get_ic_entry_size (ctx->stalker))
  {
    GumAddressSpec data_spec;

    code_slab = gum_exec_ctx_add_code_slab (ctx, gum_code_slab_new (ctx));

    gum_exec_ctx_compute_data_address_spec (ctx, data_slab->slab.size,
        &data_spec);
    if (!gum_address_spec_is_satisfied_by (&data_spec,
            gum_slab_start (&data_slab->slab)))
    {
      data_slab = gum_exec_ctx_add_data_slab (ctx, gum_data_slab_new (ctx));
    }

    gum_exec_ctx_ensure_inline_helpers_reachable (ctx);

    code_available = gum_slab_available (&code_slab->slab);
  }

  block = gum_slab_try_reserve (&data_slab->slab, sizeof (GumExecBlock));
  if (block == NULL)
  {
    data_slab = gum_exec_ctx_add_data_slab (ctx, gum_data_slab_new (ctx));
    block = gum_slab_reserve (&data_slab->slab, sizeof (GumExecBlock));
  }

  block->ctx = ctx;
  block->code_slab = code_slab;

  block->code_start = gum_slab_cursor (&code_slab->slab);

  gum_stalker_thaw (stalker, block->code_start, code_available);

  return block;
}

static void
gum_exec_block_clear (GumExecBlock * block)
{
  GumCalloutEntry * entry;

  for (entry = gum_exec_block_get_last_callout_entry (block);
      entry != NULL;
      entry = entry->next)
  {
    if (entry->data_destroy != NULL)
      entry->data_destroy (entry->data);
  }
  block->last_callout_offset = 0;

  block->storage_block = NULL;
}

static void
gum_exec_block_commit (GumExecBlock * block)
{
  GumStalker * stalker = block->ctx->stalker;
  gsize snapshot_size;

  snapshot_size =
      gum_stalker_snapshot_space_needed_for (stalker, block->real_size);
  memcpy (gum_exec_block_get_snapshot_start (block), block->real_start,
      snapshot_size);

  block->capacity = block->code_size + snapshot_size;

  gum_slab_reserve (&block->code_slab->slab, block->capacity);

  gum_stalker_freeze (stalker, block->code_start, block->code_size);
}

static void
gum_exec_block_invalidate (GumExecBlock * block)
{
  GumExecCtx * ctx = block->ctx;
  GumStalker * stalker = ctx->stalker;
  GumX86Writer * cw = &ctx->code_writer;
  const gsize max_size = GUM_INVALIDATE_TRAMPOLINE_SIZE;
  gint32 distance_to_data;

  gum_stalker_thaw (stalker, block->code_start, max_size);
  gum_x86_writer_reset (cw, block->code_start);

  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP, GUM_REG_XSP,
      -GUM_RED_ZONE_SIZE);
  gum_x86_writer_put_call_address (cw,
      GUM_ADDRESS (block->code_slab->invalidator));
  distance_to_data = (guint8 *) block - (guint8 *) GSIZE_TO_POINTER (cw->pc);
  gum_x86_writer_put_bytes (cw, (const guint8 *) &distance_to_data,
      sizeof (distance_to_data));

  gum_x86_writer_flush (cw);
  g_assert (gum_x86_writer_offset (cw) == GUM_INVALIDATE_TRAMPOLINE_SIZE);
  gum_stalker_freeze (stalker, block->code_start, max_size);
}

static gpointer
gum_exec_block_get_snapshot_start (GumExecBlock * block)
{
  return block->code_start + block->code_size;
}

static GumCalloutEntry *
gum_exec_block_get_last_callout_entry (const GumExecBlock * block)
{
  const guint last_callout_offset = block->last_callout_offset;

  if (last_callout_offset == 0)
    return NULL;

  return (GumCalloutEntry *) (block->code_start + last_callout_offset);
}

static void
gum_exec_block_set_last_callout_entry (GumExecBlock * block,
                                       GumCalloutEntry * entry)
{
  block->last_callout_offset = (guint8 *) entry - block->code_start;
}

static void
gum_exec_block_backpatch_call (GumExecBlock * block,
                               GumExecBlock * from,
                               gsize code_offset,
                               GumPrologType opened_prolog,
                               gpointer ret_real_address,
                               gsize ret_code_offset)
{
  gboolean just_unfollowed;
  GumExecCtx * ctx;
  guint8 * code_start, * ret_code_address;
  gsize code_max_size;
  GumX86Writer * cw;

  just_unfollowed = block == NULL;
  if (just_unfollowed)
    return;

  ctx = block->ctx;

  if (!gum_exec_ctx_may_now_backpatch (ctx, block))
    return;

  code_start = from->code_start + code_offset;
  ret_code_address = from->code_start + ret_code_offset;
  code_max_size = ret_code_address - code_start;

  gum_spinlock_acquire (&ctx->code_lock);

  gum_stalker_thaw (ctx->stalker, code_start, code_max_size);

  cw = &ctx->code_writer;
  gum_x86_writer_reset (cw, code_start);

  if (opened_prolog == GUM_PROLOG_NONE)
  {
    gum_x86_writer_put_pushfx (cw);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XCX);
    gum_x86_writer_put_push_reg (cw, GUM_REG_XDX);
  }

  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (ret_real_address));
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XDX,
      GUM_ADDRESS (ret_code_address));
  gum_x86_writer_put_call_address (cw,
      GUM_ADDRESS (block->ctx->last_stack_push));

  if (opened_prolog == GUM_PROLOG_NONE)
  {
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
    gum_x86_writer_put_popfx (cw);
  }
  else
  {
    gum_exec_ctx_write_epilog (block->ctx, opened_prolog, cw);
  }

  gum_x86_writer_put_push_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (ret_real_address));
  gum_x86_writer_put_xchg_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XSP);

  gum_x86_writer_put_jmp_address (cw, GUM_ADDRESS (block->code_start));

  gum_x86_writer_flush (cw);
  g_assert (gum_x86_writer_offset (cw) <= code_max_size);
  gum_stalker_freeze (ctx->stalker, code_start, code_max_size);

  gum_spinlock_release (&ctx->code_lock);

  if (ctx->observer != NULL)
  {
    GumBackpatch p;

    p.type = GUM_BACKPATCH_CALL;
    p.to = block->real_start;
    p.from = from->real_start;
    p.call.code_offset = code_offset;
    p.call.opened_prolog = opened_prolog;
    p.call.ret_real_address = ret_real_address;
    p.call.ret_code_offset = ret_code_offset;

    gum_stalker_observer_notify_backpatch (ctx->observer, &p, sizeof (p));
  }
}

static void
gum_exec_block_backpatch_jmp (GumExecBlock * block,
                              GumExecBlock * from,
                              gsize code_offset,
                              GumPrologType opened_prolog)
{
  gboolean just_unfollowed;
  GumExecCtx * ctx;
  guint8 * code_start;
  const gsize code_max_size = 128;
  GumX86Writer * cw;

  just_unfollowed = block == NULL;
  if (just_unfollowed)
    return;

  ctx = block->ctx;

  if (!gum_exec_ctx_may_now_backpatch (ctx, block))
    return;

  code_start = from->code_start + code_offset;

  gum_spinlock_acquire (&ctx->code_lock);

  gum_stalker_thaw (ctx->stalker, code_start, code_max_size);

  cw = &ctx->code_writer;
  gum_x86_writer_reset (cw, code_start);

  if (opened_prolog != GUM_PROLOG_NONE)
  {
    gum_exec_ctx_write_epilog (block->ctx, opened_prolog, cw);
  }

  gum_x86_writer_put_jmp_address (cw, GUM_ADDRESS (block->code_start));

  gum_x86_writer_flush (cw);
  gum_stalker_freeze (ctx->stalker, code_start, code_max_size);

  gum_spinlock_release (&ctx->code_lock);

  if (ctx->observer != NULL)
  {
    GumBackpatch p;

    p.type = GUM_BACKPATCH_JMP;
    p.to = block->real_start;
    p.from = from->real_start;
    p.jmp.code_offset = code_offset;
    p.jmp.opened_prolog = opened_prolog;

    gum_stalker_observer_notify_backpatch (ctx->observer, &p, sizeof (p));
  }
}

static void
gum_exec_block_backpatch_ret (GumExecBlock * block,
                              GumExecBlock * from,
                              gsize code_offset)
{
  gboolean just_unfollowed;
  GumExecCtx * ctx;
  guint8 * code_start;
  const gsize code_max_size = 128;
  GumX86Writer * cw;

  just_unfollowed = block == NULL;
  if (just_unfollowed)
    return;

  ctx = block->ctx;

  if (!gum_exec_ctx_may_now_backpatch (ctx, block))
    return;

  code_start = from->code_start + code_offset;

  gum_spinlock_acquire (&ctx->code_lock);

  gum_stalker_thaw (ctx->stalker, code_start, code_max_size);

  cw = &ctx->code_writer;
  gum_x86_writer_reset (cw, code_start);

  gum_x86_writer_put_jmp_address (cw, GUM_ADDRESS (block->code_start));

  gum_x86_writer_flush (cw);
  g_assert (gum_x86_writer_offset (cw) <= code_max_size);
  gum_stalker_freeze (ctx->stalker, code_start, code_max_size);

  gum_spinlock_release (&ctx->code_lock);

  if (ctx->observer != NULL)
  {
    GumBackpatch p;

    p.type = GUM_BACKPATCH_RET;
    p.to = block->real_start;
    p.from = from->real_start;
    p.ret.code_offset = code_offset;

    gum_stalker_observer_notify_backpatch (ctx->observer, &p, sizeof (p));
  }
}

static void
gum_exec_block_backpatch_inline_cache (GumExecBlock * block,
                                       GumExecBlock * from,
                                       gsize ic_offset)
{
  gboolean just_unfollowed;
  GumExecCtx * ctx;
  GumStalker * stalker;
  GumIcEntry * ic_entries;
  guint i;

  just_unfollowed = block == NULL;
  if (just_unfollowed)
    return;

  ctx = block->ctx;

  if (!gum_exec_ctx_may_now_backpatch (ctx, block))
    return;

  stalker = ctx->stalker;
  ic_entries = (GumIcEntry *) (from->code_start + ic_offset);

  for (i = 0; i != stalker->ic_entries; i++)
  {
    if (ic_entries[i].real_start == block->real_start)
      return;

    if (ic_entries[i].real_start != NULL)
      continue;

    gum_spinlock_acquire (&ctx->code_lock);

    gum_stalker_thaw (stalker, &ic_entries[i], sizeof (GumIcEntry));

    ic_entries[i].real_start = block->real_start;
    ic_entries[i].code_start = block->code_start;

    gum_stalker_freeze (stalker, &ic_entries[i], sizeof (GumIcEntry));

    gum_spinlock_release (&ctx->code_lock);

    if (ctx->observer != NULL)
    {
      GumBackpatch p;

      p.type = GUM_BACKPATCH_INLINE_CACHE;
      p.to = block->real_start;
      p.from = from->real_start;
      p.inline_cache.ic_offset = ic_offset;

      gum_stalker_observer_notify_backpatch (ctx->observer, &p, sizeof (p));
    }

    return;
  }
}

static GumVirtualizationRequirements
gum_exec_block_virtualize_branch_insn (GumExecBlock * block,
                                       GumGeneratorContext * gc)
{
  GumExecCtx * ctx = block->ctx;
  GumInstruction * insn = gc->instruction;
  GumX86Writer * cw = gc->code_writer;
  gboolean is_conditional;
  cs_x86 * x86 = &insn->ci->detail->x86;
  cs_x86_op * op = &x86->operands[0];
  GumBranchTarget target = { 0, };

  is_conditional =
      (insn->ci->id != X86_INS_CALL && insn->ci->id != X86_INS_JMP);

  target.origin_ip = insn->end;

  if (op->type == X86_OP_IMM)
  {
    target.absolute_address = GSIZE_TO_POINTER (op->imm);
    target.is_indirect = FALSE;
    target.pfx_seg = X86_REG_INVALID;
    target.base = X86_REG_INVALID;
    target.index = X86_REG_INVALID;
    target.scale = 0;
  }
  else if (op->type == X86_OP_MEM)
  {
#if GLIB_SIZEOF_VOID_P == 4 && defined (HAVE_WINDOWS)
    if (op->mem.segment == X86_REG_INVALID &&
        op->mem.base == X86_REG_INVALID &&
        op->mem.index == X86_REG_INVALID)
    {
      GArray * impls = ctx->stalker->wow_transition_impls;
      guint i;

      for (i = 0; i != impls->len; i++)
      {
        gpointer impl = g_array_index (impls, gpointer, i);

        if (GSIZE_TO_POINTER (op->mem.disp) == impl)
          return gum_exec_block_virtualize_wow64_transition (block, gc, impl);
      }
    }
#endif

#ifdef HAVE_WINDOWS
    /* Can't follow WoW64 */
    if (op->mem.segment == X86_REG_FS && op->mem.disp == 0xc0)
      return GUM_REQUIRE_SINGLE_STEP;
#endif

    if (op->mem.base == X86_REG_INVALID && op->mem.index == X86_REG_INVALID)
      target.absolute_address = GSIZE_TO_POINTER (op->mem.disp);
    else
      target.relative_offset = op->mem.disp;

    target.is_indirect = TRUE;
    target.pfx_seg = op->mem.segment;
    target.base = op->mem.base;
    target.index = op->mem.index;
    target.scale = op->mem.scale;
  }
  else if (op->type == X86_OP_REG)
  {
    target.is_indirect = FALSE;
    target.pfx_seg = X86_REG_INVALID;
    target.base = op->reg;
    target.index = X86_REG_INVALID;
    target.scale = 0;
  }
  else
  {
    g_assert_not_reached ();
  }

  if (insn->ci->id == X86_INS_CALL)
  {
    gboolean target_is_excluded = FALSE;

    if ((ctx->sink_mask & GUM_CALL) != 0)
    {
      gum_exec_block_write_call_event_code (block, &target, gc,
          GUM_CODE_INTERRUPTIBLE);
    }

    if (!target.is_indirect && target.base == X86_REG_INVALID &&
        ctx->activation_target == NULL)
    {
      target_is_excluded =
          gum_stalker_is_excluding (ctx->stalker, target.absolute_address);
    }

    if (target_is_excluded)
    {
      GumBranchTarget next_instruction = { 0, };

      gum_exec_block_open_prolog (block, GUM_PROLOG_IC, gc);
      gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
          GUM_ADDRESS (insn->end));
      gum_x86_writer_put_mov_near_ptr_reg (cw,
          GUM_ADDRESS (&ctx->pending_return_location), GUM_REG_XAX);
      gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
          GUM_ADDRESS (&ctx->pending_calls));
      gum_x86_writer_put_inc_reg_ptr (cw, GUM_PTR_DWORD, GUM_REG_XAX);
      gum_exec_block_close_prolog (block, gc);

      gum_x86_relocator_write_one_no_label (gc->relocator);

      gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

      gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
          GUM_ADDRESS (&ctx->pending_calls));
      gum_x86_writer_put_dec_reg_ptr (cw, GUM_PTR_DWORD, GUM_REG_XAX);

      next_instruction.is_indirect = FALSE;
      next_instruction.absolute_address = insn->end;
      gum_exec_block_write_jmp_transfer_code (block, &next_instruction,
          GUM_ENTRYGATE (excluded_call_imm), gc);

      return GUM_REQUIRE_NOTHING;
    }

    gum_x86_relocator_skip_one_no_label (gc->relocator);
    gum_exec_block_write_call_invoke_code (block, &target, gc);
  }
  else if (insn->ci->id == X86_INS_JECXZ || insn->ci->id == X86_INS_JRCXZ)
  {
    gpointer is_true, is_false;
    GumBranchTarget false_target = { 0, };

    gum_x86_relocator_skip_one_no_label (gc->relocator);

    is_true =
        GUINT_TO_POINTER ((GPOINTER_TO_UINT (insn->start) << 16) | 0xbeef);
    is_false =
        GUINT_TO_POINTER ((GPOINTER_TO_UINT (insn->start) << 16) | 0xbabe);

    gum_exec_block_close_prolog (block, gc);

    gum_x86_writer_put_jcc_short_label (cw, X86_INS_JCXZ, is_true, GUM_NO_HINT);
    gum_x86_writer_put_jmp_near_label (cw, is_false);

    gum_x86_writer_put_label (cw, is_true);
    gum_exec_block_write_jmp_transfer_code (block, &target,
        GUM_ENTRYGATE (jmp_cond_jcxz), gc);

    gum_x86_writer_put_label (cw, is_false);
    false_target.is_indirect = FALSE;
    false_target.absolute_address = insn->end;
    gum_exec_block_write_jmp_transfer_code (block, &false_target,
        GUM_ENTRYGATE (jmp_cond_jcxz), gc);
  }
  else
  {
    gpointer is_false;
    GumExecCtxReplaceCurrentBlockFunc regular_entry_func, cond_entry_func;

    gum_x86_relocator_skip_one_no_label (gc->relocator);

    is_false =
        GUINT_TO_POINTER ((GPOINTER_TO_UINT (insn->start) << 16) | 0xbeef);

    if (is_conditional)
    {
      g_assert (!target.is_indirect);

      gum_exec_block_close_prolog (block, gc);

      gum_x86_writer_put_jcc_near_label (cw, gum_negate_jcc (insn->ci->id),
          is_false, GUM_NO_HINT);
    }

    if (target.is_indirect)
    {
      regular_entry_func = GUM_ENTRYGATE (jmp_mem);
      cond_entry_func = GUM_ENTRYGATE (jmp_cond_mem);
    }
    else if (target.base != X86_REG_INVALID)
    {
      regular_entry_func = GUM_ENTRYGATE (jmp_reg);
      cond_entry_func = GUM_ENTRYGATE (jmp_cond_reg);
    }
    else
    {
      regular_entry_func = GUM_ENTRYGATE (jmp_imm);
      cond_entry_func = GUM_ENTRYGATE (jmp_cond_imm);
    }

    gum_exec_block_write_jmp_transfer_code (block, &target,
        is_conditional ? cond_entry_func : regular_entry_func, gc);

    if (is_conditional)
    {
      GumBranchTarget cond_target = { 0, };

      cond_target.is_indirect = FALSE;
      cond_target.absolute_address = insn->end;

      gum_x86_writer_put_label (cw, is_false);
      gum_exec_block_write_jmp_transfer_code (block, &cond_target,
          cond_entry_func, gc);
    }
  }

  return GUM_REQUIRE_NOTHING;
}

static GumVirtualizationRequirements
gum_exec_block_virtualize_ret_insn (GumExecBlock * block,
                                    GumGeneratorContext * gc)
{
  if ((block->ctx->sink_mask & GUM_RET) != 0)
    gum_exec_block_write_ret_event_code (block, gc, GUM_CODE_INTERRUPTIBLE);

  gum_x86_relocator_skip_one_no_label (gc->relocator);

  gum_exec_block_write_ret_transfer_code (block, gc);

  return GUM_REQUIRE_NOTHING;
}

static GumVirtualizationRequirements
gum_exec_block_virtualize_sysenter_insn (GumExecBlock * block,
                                         GumGeneratorContext * gc)
{
#if GLIB_SIZEOF_VOID_P == 4 && !defined (HAVE_QNX)
  GumX86Writer * cw = gc->code_writer;
#if defined (HAVE_WINDOWS)
  guint8 code[] = {
    /* 00 */ 0x50,                                /* push eax              */
    /* 01 */ 0x8b, 0x02,                          /* mov eax, [edx]        */
    /* 03 */ 0xa3, 0xaa, 0xaa, 0xaa, 0xaa,        /* mov [0xaaaaaaaa], eax */
    /* 08 */ 0xc7, 0x02, 0xbb, 0xbb, 0xbb, 0xbb,  /* mov [edx], 0xbbbbbbbb */
    /* 0e */ 0x58,                                /* pop eax               */
    /* 0f */ 0x0f, 0x34,                          /* sysenter              */
    /* 11 */ 0xcc, 0xcc, 0xcc, 0xcc               /* <saved ret-addr here> */
  };
  const gsize store_ret_addr_offset = 0x03 + 1;
  const gsize load_continuation_addr_offset = 0x08 + 2;
  const gsize saved_ret_addr_offset = 0x11;
#elif defined (HAVE_DARWIN)
  guint8 code[] = {
    /* 00 */ 0x89, 0x15, 0xaa, 0xaa, 0xaa, 0xaa, /* mov [0xaaaaaaaa], edx */
    /* 06 */ 0xba, 0xbb, 0xbb, 0xbb, 0xbb,       /* mov edx, 0xbbbbbbbb   */
    /* 0b */ 0x0f, 0x34,                         /* sysenter              */
    /* 0d */ 0xcc, 0xcc, 0xcc, 0xcc              /* <saved ret-addr here> */
  };
  const gsize store_ret_addr_offset = 0x00 + 2;
  const gsize load_continuation_addr_offset = 0x06 + 1;
  const gsize saved_ret_addr_offset = 0x0d;
#elif defined (HAVE_LINUX)
  guint8 code[] = {
    /* 00 */ 0x8b, 0x54, 0x24, 0x0c,             /* mov edx, [esp + 12]   */
    /* 04 */ 0x89, 0x15, 0xaa, 0xaa, 0xaa, 0xaa, /* mov [0xaaaaaaaa], edx */
    /* 0a */ 0xba, 0xbb, 0xbb, 0xbb, 0xbb,       /* mov edx, 0xbbbbbbbb   */
    /* 0f */ 0x89, 0x54, 0x24, 0x0c,             /* mov [esp + 12], edx   */
    /* 13 */ 0x8b, 0x54, 0x24, 0x04,             /* mov edx, [esp + 4]    */
    /* 17 */ 0x0f, 0x34,                         /* sysenter              */
    /* 19 */ 0xcc, 0xcc, 0xcc, 0xcc              /* <saved ret-addr here> */
  };
  const gsize store_ret_addr_offset = 0x04 + 2;
  const gsize load_continuation_addr_offset = 0x0a + 1;
  const gsize saved_ret_addr_offset = 0x19;
#endif
  gpointer * saved_ret_addr;
  gpointer continuation;

  gum_exec_block_close_prolog (block, gc);

  saved_ret_addr = GSIZE_TO_POINTER (cw->pc + saved_ret_addr_offset);
  continuation = GSIZE_TO_POINTER (cw->pc + saved_ret_addr_offset + 4);
  *((gpointer *) (code + store_ret_addr_offset)) = saved_ret_addr;
  *((gpointer *) (code + load_continuation_addr_offset)) = continuation;

  gum_x86_writer_put_bytes (cw, code, sizeof (code));

  gum_exec_block_write_sysenter_continuation_code (block, gc, saved_ret_addr);

  return GUM_REQUIRE_NOTHING;
#else
  return GUM_REQUIRE_RELOCATION;
#endif
}

#if GLIB_SIZEOF_VOID_P == 4 && defined (HAVE_WINDOWS)

static GumVirtualizationRequirements
gum_exec_block_virtualize_wow64_transition (GumExecBlock * block,
                                            GumGeneratorContext * gc,
                                            gpointer impl)
{
  GumX86Writer * cw = gc->code_writer;
  guint8 code[] = {
    /* 00 */ 0x50,                        /* push eax */
    /* 01 */ 0x8b, 0x44, 0x24, 0x04,      /* mov eax, dword [esp + 4] */
    /* 05 */ 0x89, 0x05, 0xaa, 0xaa, 0xaa,
             0xaa,                        /* mov dword [0xaaaaaaaa], eax */
    /* 0b */ 0xc7, 0x44, 0x24, 0x04, 0xbb,
             0xbb, 0xbb, 0xbb,            /* mov dword [esp + 4], 0xbbbbbbbb */
    /* 13 */ 0x58,                        /* pop eax */
    /* 14 */ 0xff, 0x25, 0xcc, 0xcc, 0xcc,
             0xcc,                        /* jmp dword [0xcccccccc] */
    /* 1a */ 0x90, 0x90, 0x90, 0x90       /* <saved ret-addr here> */
  };
  const gsize store_ret_addr_offset = 0x05 + 2;
  const gsize load_continuation_addr_offset = 0x0b + 4;
  const gsize wow64_transition_addr_offset = 0x14 + 2;
  const gsize saved_ret_addr_offset = 0x1a;

  gum_exec_block_close_prolog (block, gc);

  gpointer * saved_ret_addr = GSIZE_TO_POINTER (cw->pc + saved_ret_addr_offset);
  gpointer continuation = GSIZE_TO_POINTER (cw->pc + saved_ret_addr_offset + 4);

  *((gpointer *) (code + store_ret_addr_offset)) = saved_ret_addr;
  *((gpointer *) (code + load_continuation_addr_offset)) = continuation;
  *((gpointer *) (code + wow64_transition_addr_offset)) = impl;

  gum_x86_writer_put_bytes (cw, code, sizeof (code));

  gum_exec_block_write_sysenter_continuation_code (block, gc, saved_ret_addr);

  return GUM_REQUIRE_NOTHING;
}

#endif

static void
gum_exec_block_write_call_invoke_code (GumExecBlock * block,
                                       const GumBranchTarget * target,
                                       GumGeneratorContext * gc)
{
  GumStalker * stalker = block->ctx->stalker;
  const gint trust_threshold = stalker->trust_threshold;
  GumX86Writer * cw = gc->code_writer;
  const GumAddress call_code_start = cw->pc;
  const GumPrologType opened_prolog = gc->opened_prolog;
  gboolean can_backpatch_statically;
  GumIcEntry * ic_entries = NULL;
  gpointer * ic_match = NULL;
  GumExecCtxReplaceCurrentBlockFunc entry_func;
  gconstpointer push_application_retaddr = cw->code + 1;
  gconstpointer perform_stack_push = cw->code + 2;
  gconstpointer look_in_cache = cw->code + 3;
  gconstpointer loop = cw->code + 4;
  gconstpointer try_next = cw->code + 5;
  gconstpointer resolve_dynamically = cw->code + 6;
  gconstpointer beach = cw->code + 7;

  GumAddress ret_real_address, ret_code_address;

  can_backpatch_statically =
      trust_threshold >= 0 &&
      !target->is_indirect &&
      target->base == X86_REG_INVALID;

  if (trust_threshold >= 0 && !can_backpatch_statically)
  {
    gpointer null_ptr = NULL;
    gsize empty_val = GUM_IC_MAGIC_EMPTY;
    gsize scratch_val = GUM_IC_MAGIC_SCRATCH;
    guint i;

    if (opened_prolog == GUM_PROLOG_NONE)
    {
      gum_exec_block_open_prolog (block, GUM_PROLOG_IC, gc);
      gum_x86_writer_put_push_reg (cw, GUM_REG_XCX);
      gum_x86_writer_put_push_reg (cw, GUM_REG_XDX);
    }

    gum_x86_writer_put_call_near_label (cw, push_application_retaddr);
    gc->accumulated_stack_delta += sizeof (gpointer);

    gum_x86_writer_put_call_near_label (cw, perform_stack_push);

    if (opened_prolog == GUM_PROLOG_NONE)
    {
      gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);
      gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
    }
    else
    {
      gum_exec_block_close_prolog (block, gc);
      gum_exec_block_open_prolog (block, GUM_PROLOG_IC, gc);
      gc->accumulated_stack_delta += sizeof (gpointer);
    }

    /*
     * We need to use a near rather than short jump since our inline cache is
     * larger than the maximum distance of a short jump (-128 to +127).
     */
    gum_x86_writer_put_jmp_near_label (cw, look_in_cache);

    ic_entries = gum_x86_writer_cur (cw);

    for (i = 0; i != stalker->ic_entries; i++)
    {
      gum_x86_writer_put_bytes (cw, (guint8 *) &null_ptr, sizeof (null_ptr));
      gum_x86_writer_put_bytes (cw, (guint8 *) &empty_val, sizeof (empty_val));
    }

    /*
     * Write a token which we can replace with our matched ic entry code_start
     * so we can use it as scratch space and retrieve and jump to it once we
     * have restored the target application context.
     */
    ic_match = gum_x86_writer_cur (cw);
    gum_x86_writer_put_bytes (cw, (guint8 *) &scratch_val,
        sizeof (scratch_val));

    gum_x86_writer_put_label (cw, look_in_cache);

    gum_x86_writer_put_push_reg (cw, GUM_REG_XCX);
    gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);

    gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
        GUM_ADDRESS (ic_entries));
    gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XBX,
        GUM_ADDRESS (&ic_entries[stalker->ic_entries]));

    /*
     * Write our inline assembly which iterates through the IcEntry structures,
     * attempting to match the real_start member with the target block address.
     */
    gum_x86_writer_put_label (cw, loop);
    gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XCX);

    /* If real_start != target block, then continue */
    gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw, GUM_REG_XSP, 0, GUM_REG_XAX);
    gum_x86_writer_put_jcc_short_label (cw, X86_INS_JNE, try_next,
        GUM_NO_HINT);

    /*
     * If real_start == NULL, then break: we have reached the end of the
     * initialized IcEntry structures.
     */
    gum_x86_writer_put_cmp_reg_i32 (cw, GUM_REG_XAX, 0);
    gum_x86_writer_put_jcc_short_label (cw, X86_INS_JE, resolve_dynamically,
        GUM_NO_HINT);

    /* We found a match, stash the code_start value in the ic_match */
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, GUM_REG_XCX, GUM_REG_XCX,
        G_STRUCT_OFFSET (GumIcEntry, code_start));
    gum_x86_writer_put_mov_near_ptr_reg (cw, GUM_ADDRESS (ic_match),
        GUM_REG_XCX);

    /* Restore the target context and jump at ic_match */
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
    gum_exec_ctx_write_epilog (block->ctx, GUM_PROLOG_IC, cw);
    gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (ic_match));

    /* Increment our position through the IcEntry array */
    gum_x86_writer_put_label (cw, try_next);
    gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XCX, sizeof (GumIcEntry));
    gum_x86_writer_put_cmp_reg_reg (cw, GUM_REG_XCX, GUM_REG_XBX);
    gum_x86_writer_put_jcc_short_label (cw, X86_INS_JLE, loop,
        GUM_LIKELY);

    /* Cache miss, do it the hard way */
    gum_x86_writer_put_label (cw, resolve_dynamically);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
    gum_exec_block_close_prolog (block, gc);

  }

  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  if (ic_entries == NULL)
  {
    gum_x86_writer_put_call_near_label (cw, push_application_retaddr);

    gum_x86_writer_put_call_near_label (cw, perform_stack_push);
  }

  gc->accumulated_stack_delta += sizeof (gpointer);

  if (target->is_indirect)
  {
    entry_func = GUM_ENTRYGATE (call_mem);
  }
  else if (target->base != X86_REG_INVALID)
  {
    entry_func = GUM_ENTRYGATE (call_reg);
  }
  else
  {
    entry_func = GUM_ENTRYGATE (call_imm);
  }

  /* Generate code for the target */
  gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);
  gum_x86_writer_put_pop_reg (cw, GUM_THUNK_REG_ARG1);
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (block->ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (entry_func));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_reg (cw, GUM_REG_XDX, GUM_REG_XAX);
  gum_x86_writer_put_jmp_near_label (cw, beach);

  /* Generate code for handling the return */
  ret_real_address = GUM_ADDRESS (gc->instruction->end);
  ret_code_address = cw->pc;

  gum_exec_ctx_write_prolog (block->ctx, GUM_PROLOG_MINIMAL, cw);

  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG1,
      ret_real_address);
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (block->ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (GUM_ENTRYGATE (post_call_invoke)));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);

  if (trust_threshold >= 0)
  {
    gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
        GUM_ADDRESS (&block->ctx->current_block));
    gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
        GUM_ADDRESS (gum_exec_block_backpatch_ret), 3,
        GUM_ARG_REGISTER, GUM_REG_XAX,
        GUM_ARG_ADDRESS, GUM_ADDRESS (block),
        GUM_ARG_ADDRESS, ret_code_address - GUM_ADDRESS (block->code_start));
  }

  gum_exec_ctx_write_epilog (block->ctx, GUM_PROLOG_MINIMAL, cw);
  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->resume_at));

  gum_x86_writer_put_label (cw, push_application_retaddr);
  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
      GUM_ADDRESS (&block->ctx->app_stack));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XAX, sizeof (gpointer));
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (gc->instruction->end));
  gum_x86_writer_put_mov_reg_ptr_reg (cw, GUM_REG_XAX, GUM_REG_XCX);
  gum_x86_writer_put_mov_near_ptr_reg (cw,
      GUM_ADDRESS (&block->ctx->app_stack), GUM_REG_XAX);
  gum_x86_writer_put_ret (cw);

  gum_x86_writer_put_label (cw, perform_stack_push);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX, ret_real_address);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XDX, ret_code_address);
  gum_x86_writer_put_call_address (cw,
      GUM_ADDRESS (block->ctx->last_stack_push));
  gum_x86_writer_put_ret (cw);

  gum_x86_writer_put_label (cw, beach);

  if (trust_threshold >= 0)
  {
    gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
        GUM_ADDRESS (&block->ctx->current_block));
  }

  if (can_backpatch_statically)
  {
    gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
        GUM_ADDRESS (gum_exec_block_backpatch_call), 6,
        GUM_ARG_REGISTER, GUM_REG_XAX,
        GUM_ARG_ADDRESS, GUM_ADDRESS (block),
        GUM_ARG_ADDRESS, call_code_start - GUM_ADDRESS (block->code_start),
        GUM_ARG_ADDRESS, GUM_ADDRESS (opened_prolog),
        GUM_ARG_ADDRESS, GUM_ADDRESS (ret_real_address),
        GUM_ARG_ADDRESS, ret_code_address - GUM_ADDRESS (block->code_start));
  }

  if (ic_entries != NULL)
  {
    gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
        GUM_ADDRESS (gum_exec_block_backpatch_inline_cache), 3,
        GUM_ARG_REGISTER, GUM_REG_XAX,
        GUM_ARG_ADDRESS, GUM_ADDRESS (block),
        GUM_ARG_ADDRESS, GUM_ADDRESS (ic_entries) -
            GUM_ADDRESS (block->code_start));
  }

  /* Execute the generated code */
  gum_exec_block_close_prolog (block, gc);

  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->resume_at));
}

static void
gum_exec_block_write_jmp_transfer_code (GumExecBlock * block,
                                        const GumBranchTarget * target,
                                        GumExecCtxReplaceCurrentBlockFunc func,
                                        GumGeneratorContext * gc)
{
  GumStalker * stalker = block->ctx->stalker;
  const gint trust_threshold = stalker->trust_threshold;
  GumX86Writer * cw = gc->code_writer;
  const GumAddress code_start = cw->pc;
  const GumPrologType opened_prolog = gc->opened_prolog;
  gboolean can_backpatch_statically;
  GumIcEntry * ic_entries = NULL;
  gpointer * ic_match = NULL;
  gconstpointer look_in_cache = cw->code + 1;
  gconstpointer loop = cw->code + 2;
  gconstpointer try_next = cw->code + 3;
  gconstpointer resolve_dynamically = cw->code + 4;

  can_backpatch_statically =
      trust_threshold >= 0 &&
      !target->is_indirect &&
      target->base == X86_REG_INVALID;

  if (trust_threshold >= 0 && !can_backpatch_statically)
  {
    guint i;
    gpointer null_ptr = NULL;
    gsize empty_val = GUM_IC_MAGIC_EMPTY;
    gsize scratch_val = GUM_IC_MAGIC_SCRATCH;

    gum_exec_block_close_prolog (block, gc);

    /*
     * We need to use a near rather than short jump since our inline cache is
     * larger than the maximum distance of a short jump (-128 to +127).
     */
    gum_x86_writer_put_jmp_near_label (cw, look_in_cache);

    ic_entries = gum_x86_writer_cur (cw);

    for (i = 0; i != stalker->ic_entries; i++)
    {
      gum_x86_writer_put_bytes (cw, (guint8 *) &null_ptr, sizeof (null_ptr));
      gum_x86_writer_put_bytes (cw, (guint8 *) &empty_val, sizeof (empty_val));
    }

    /*
     * Write a token which we can replace with our matched ic entry code_start
     * so we can use it as scratch space and retrieve and jump to it once we
     * have restored the target application context.
     */
    ic_match = gum_x86_writer_cur (cw);
    gum_x86_writer_put_bytes (cw, (guint8 *) &scratch_val,
        sizeof (scratch_val));

    gum_x86_writer_put_label (cw, look_in_cache);
    gum_exec_block_open_prolog (block, GUM_PROLOG_IC, gc);

    gum_x86_writer_put_push_reg (cw, GUM_REG_XCX);
    gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);

    gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
        GUM_ADDRESS (ic_entries));
    gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XBX,
        GUM_ADDRESS (&ic_entries[stalker->ic_entries]));

    /*
     * Write our inline assembly which iterates through the IcEntry structures,
     * attempting to match the real_start member with the target block address.
     */
    gum_x86_writer_put_label (cw, loop);
    gum_x86_writer_put_mov_reg_reg_ptr (cw, GUM_REG_XAX, GUM_REG_XCX);

    /* If real_start != target block, then continue */
    gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw, GUM_REG_XSP, 0, GUM_REG_XAX);
    gum_x86_writer_put_jcc_short_label (cw, X86_INS_JNE, try_next,
        GUM_NO_HINT);

    /*
     * If real_start == NULL, then break: we have reached the end of the
     * initialized IcEntry structures.
     */
    gum_x86_writer_put_cmp_reg_i32 (cw, GUM_REG_XAX, 0);
    gum_x86_writer_put_jcc_short_label (cw, X86_INS_JE, resolve_dynamically,
        GUM_NO_HINT);

    /* We found a match, stash the code_start value in the ic_match */
    gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, GUM_REG_XCX, GUM_REG_XCX,
        G_STRUCT_OFFSET (GumIcEntry, code_start));
    gum_x86_writer_put_mov_near_ptr_reg (cw, GUM_ADDRESS (ic_match),
        GUM_REG_XCX);

    /* Restore the target context and jump at ic_match */
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
    gum_exec_ctx_write_epilog (block->ctx, GUM_PROLOG_IC, cw);
    gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (ic_match));

    /* Increment our position through the IcEntry array */
    gum_x86_writer_put_label (cw, try_next);
    gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XCX, sizeof (GumIcEntry));
    gum_x86_writer_put_cmp_reg_reg (cw, GUM_REG_XCX, GUM_REG_XBX);
    gum_x86_writer_put_jcc_short_label (cw, X86_INS_JLE, loop, GUM_NO_HINT);

    /* Cache miss, do it the hard way */
    gum_x86_writer_put_label (cw, resolve_dynamically);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XAX);
    gum_x86_writer_put_pop_reg (cw, GUM_REG_XCX);
    gum_exec_block_close_prolog (block, gc);
  }

  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);
  gum_x86_writer_put_pop_reg (cw, GUM_THUNK_REG_ARG1);
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (block->ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX, GUM_ADDRESS (func));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);

  if (trust_threshold >= 0)
  {
    gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_XAX,
        GUM_ADDRESS (&block->ctx->current_block));
  }

  if (can_backpatch_statically)
  {
    gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
        GUM_ADDRESS (gum_exec_block_backpatch_jmp), 4,
        GUM_ARG_REGISTER, GUM_REG_XAX,
        GUM_ARG_ADDRESS, GUM_ADDRESS (block),
        GUM_ARG_ADDRESS, code_start - GUM_ADDRESS (block->code_start),
        GUM_ARG_ADDRESS, GUM_ADDRESS (opened_prolog));
  }

  if (ic_entries != NULL)
  {
    gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
        GUM_ADDRESS (gum_exec_block_backpatch_inline_cache), 3,
        GUM_ARG_REGISTER, GUM_REG_XAX,
        GUM_ARG_ADDRESS, GUM_ADDRESS (block),
        GUM_ARG_ADDRESS, GUM_ADDRESS (ic_entries) -
            GUM_ADDRESS (block->code_start));
  }

  gum_exec_block_close_prolog (block, gc);

  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->resume_at));
}

static void
gum_exec_block_write_ret_transfer_code (GumExecBlock * block,
                                        GumGeneratorContext * gc)
{
  GumX86Writer * cw = gc->code_writer;

  gum_exec_block_close_prolog (block, gc);

  gum_x86_writer_put_lea_reg_reg_offset (cw, GUM_REG_XSP,
      GUM_REG_XSP, -GUM_RED_ZONE_SIZE);
  gum_x86_writer_put_push_reg (cw, GUM_REG_XCX);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XCX,
      GUM_ADDRESS (gc->instruction->start));
  gum_x86_writer_put_jmp_address (cw,
      GUM_ADDRESS (block->ctx->last_stack_pop_and_go));
}

static void
gum_exec_block_write_single_step_transfer_code (GumExecBlock * block,
                                                GumGeneratorContext * gc)
{
  guint8 code[] = {
    0xc6, 0x05, 0x78, 0x56, 0x34, 0x12,       /* mov byte [X], state */
          GUM_EXEC_CTX_SINGLE_STEPPING_ON_CALL,
    0x9c,                                     /* pushfd              */
    0x81, 0x0c, 0x24, 0x00, 0x01, 0x00, 0x00, /* or [esp], 0x100     */
    0x9d                                      /* popfd               */
  };

  *((GumExecCtxMode **) (code + 2)) = &block->ctx->mode;
  gum_x86_writer_put_bytes (gc->code_writer, code, sizeof (code));
  gum_x86_writer_put_jmp_address (gc->code_writer,
      GUM_ADDRESS (gc->instruction->start));
}

#if GLIB_SIZEOF_VOID_P == 4 && !defined (HAVE_QNX)

static void
gum_exec_block_write_sysenter_continuation_code (GumExecBlock * block,
                                                 GumGeneratorContext * gc,
                                                 gpointer saved_ret_addr)
{
  GumX86Writer * cw = gc->code_writer;
  gconstpointer resolve_dynamically_label = cw->code;

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_EDX,
      GUM_ADDRESS (saved_ret_addr));

  if ((block->ctx->sink_mask & GUM_RET) != 0)
  {
    gum_exec_block_write_ret_event_code (block, gc, GUM_CODE_UNINTERRUPTIBLE);
    gum_exec_block_close_prolog (block, gc);
  }

  /*
   * Fast path (try the stack)
   */
  gum_x86_writer_put_pushfx (cw);
  gum_x86_writer_put_push_reg (cw, GUM_REG_EAX);

  /* But first, check if we've been asked to unfollow,
   * in which case we'll enter the Stalker so the unfollow can
   * be completed... */
  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_EAX,
      GUM_ADDRESS (&block->ctx->state));
  gum_x86_writer_put_cmp_reg_i32 (cw, GUM_REG_EAX,
      GUM_EXEC_CTX_UNFOLLOW_PENDING);
  gum_x86_writer_put_jcc_short_label (cw, X86_INS_JE,
      resolve_dynamically_label, GUM_UNLIKELY);

  /* Check frame at the top of the stack */
  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_REG_EAX,
      GUM_ADDRESS (&block->ctx->current_frame));
  gum_x86_writer_put_cmp_reg_offset_ptr_reg (cw,
      GUM_REG_EAX, G_STRUCT_OFFSET (GumExecFrame, real_address),
      GUM_REG_EDX);
  gum_x86_writer_put_jcc_short_label (cw, X86_INS_JNE,
      resolve_dynamically_label, GUM_UNLIKELY);

  /* Replace return address */
  gum_x86_writer_put_mov_reg_reg_offset_ptr (cw, GUM_REG_EDX,
      GUM_REG_EAX, G_STRUCT_OFFSET (GumExecFrame, code_address));

  /* Pop from our stack */
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_EAX, sizeof (GumExecFrame));
  gum_x86_writer_put_mov_near_ptr_reg (cw,
      GUM_ADDRESS (&block->ctx->current_frame), GUM_REG_EAX);

  /* Proceeed to block */
  gum_x86_writer_put_pop_reg (cw, GUM_REG_EAX);
  gum_x86_writer_put_popfx (cw);
  gum_x86_writer_put_jmp_reg (cw, GUM_REG_EDX);

  gum_x86_writer_put_label (cw, resolve_dynamically_label);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_EAX);
  gum_x86_writer_put_popfx (cw);

  /*
   * Slow path (resolve dynamically)
   */
  gum_exec_block_open_prolog (block, GUM_PROLOG_MINIMAL, gc);

  gum_x86_writer_put_mov_reg_near_ptr (cw, GUM_THUNK_REG_ARG1,
      GUM_ADDRESS (saved_ret_addr));
  gum_x86_writer_put_mov_reg_address (cw, GUM_THUNK_REG_ARG0,
      GUM_ADDRESS (block->ctx));
  gum_x86_writer_put_sub_reg_imm (cw, GUM_REG_ESP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);
  gum_x86_writer_put_mov_reg_address (cw, GUM_REG_XAX,
      GUM_ADDRESS (GUM_ENTRYGATE (sysenter_slow_path)));
  gum_x86_writer_put_call_reg (cw, GUM_REG_XAX);
  gum_x86_writer_put_add_reg_imm (cw, GUM_REG_XSP,
      GUM_THUNK_ARGLIST_STACK_RESERVE);

  gum_exec_block_close_prolog (block, gc);
  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&block->ctx->resume_at));

  gum_x86_relocator_skip_one_no_label (gc->relocator);
}

#endif

static void
gum_exec_block_write_call_event_code (GumExecBlock * block,
                                      const GumBranchTarget * target,
                                      GumGeneratorContext * gc,
                                      GumCodeContext cc)
{
  GumX86Writer * cw = gc->code_writer;

  gum_exec_block_open_prolog (block, GUM_PROLOG_FULL, gc);

  gum_exec_ctx_write_push_branch_target_address (block->ctx, target, gc);
  gum_x86_writer_put_pop_reg (cw, GUM_REG_XDX);

  gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
      GUM_ADDRESS (gum_exec_ctx_emit_call_event), 4,
      GUM_ARG_ADDRESS, GUM_ADDRESS (block->ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (gc->instruction->start),
      GUM_ARG_REGISTER, GUM_REG_XDX,
      GUM_ARG_REGISTER, GUM_REG_XBX);

  gum_exec_block_write_unfollow_check_code (block, gc, cc);
}

static void
gum_exec_block_write_ret_event_code (GumExecBlock * block,
                                     GumGeneratorContext * gc,
                                     GumCodeContext cc)
{
  gum_exec_block_open_prolog (block, GUM_PROLOG_FULL, gc);

  gum_x86_writer_put_call_address_with_aligned_arguments (gc->code_writer,
      GUM_CALL_CAPI, GUM_ADDRESS (gum_exec_ctx_emit_ret_event), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (block->ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (gc->instruction->start),
      GUM_ARG_REGISTER, GUM_REG_XBX);

  gum_exec_block_write_unfollow_check_code (block, gc, cc);
}

static void
gum_exec_block_write_exec_event_code (GumExecBlock * block,
                                      GumGeneratorContext * gc,
                                      GumCodeContext cc)
{
  gum_exec_block_open_prolog (block, GUM_PROLOG_FULL, gc);

  gum_x86_writer_put_call_address_with_aligned_arguments (gc->code_writer,
      GUM_CALL_CAPI, GUM_ADDRESS (gum_exec_ctx_emit_exec_event), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (block->ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (gc->instruction->start),
      GUM_ARG_REGISTER, GUM_REG_XBX);

  gum_exec_block_write_unfollow_check_code (block, gc, cc);
}

static void
gum_exec_block_write_block_event_code (GumExecBlock * block,
                                       GumGeneratorContext * gc,
                                       GumCodeContext cc)
{
  gum_exec_block_open_prolog (block, GUM_PROLOG_FULL, gc);

  gum_x86_writer_put_call_address_with_aligned_arguments (gc->code_writer,
      GUM_CALL_CAPI, GUM_ADDRESS (gum_exec_ctx_emit_block_event), 3,
      GUM_ARG_ADDRESS, GUM_ADDRESS (block->ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (block),
      GUM_ARG_REGISTER, GUM_REG_XBX);

  gum_exec_block_write_unfollow_check_code (block, gc, cc);
}

static void
gum_exec_block_write_unfollow_check_code (GumExecBlock * block,
                                          GumGeneratorContext * gc,
                                          GumCodeContext cc)
{
  GumExecCtx * ctx = block->ctx;
  GumX86Writer * cw = gc->code_writer;
  gconstpointer beach = cw->code + 1;
  GumPrologType opened_prolog;

  if (cc != GUM_CODE_INTERRUPTIBLE)
    return;

  gum_x86_writer_put_call_address_with_aligned_arguments (cw, GUM_CALL_CAPI,
      GUM_ADDRESS (gum_exec_ctx_maybe_unfollow), 2,
      GUM_ARG_ADDRESS, GUM_ADDRESS (ctx),
      GUM_ARG_ADDRESS, GUM_ADDRESS (gc->instruction->start));
  gum_x86_writer_put_test_reg_reg (cw, GUM_REG_EAX, GUM_REG_EAX);
  gum_x86_writer_put_jcc_near_label (cw, X86_INS_JE, beach, GUM_LIKELY);

  opened_prolog = gc->opened_prolog;
  gum_exec_block_close_prolog (block, gc);
  gc->opened_prolog = opened_prolog;

  gum_x86_writer_put_jmp_near_ptr (cw, GUM_ADDRESS (&ctx->resume_at));

  gum_x86_writer_put_label (cw, beach);
}

static void
gum_exec_block_maybe_write_call_probe_code (GumExecBlock * block,
                                            GumGeneratorContext * gc)
{
  GumStalker * stalker = block->ctx->stalker;

  if (!stalker->any_probes_attached)
    return;

  gum_spinlock_acquire (&stalker->probe_lock);

  if (g_hash_table_contains (stalker->probe_array_by_address,
          block->real_start))
  {
    gum_exec_block_write_call_probe_code (block, gc);
  }

  gum_spinlock_release (&stalker->probe_lock);
}

static void
gum_exec_block_write_call_probe_code (GumExecBlock * block,
                                      GumGeneratorContext * gc)
{
  g_assert (gc->opened_prolog == GUM_PROLOG_NONE);
  gum_exec_block_open_prolog (block, GUM_PROLOG_FULL, gc);

  gum_x86_writer_put_call_address_with_aligned_arguments (gc->code_writer,
      GUM_CALL_CAPI, GUM_ADDRESS (gum_exec_block_invoke_call_probes),
      2,
      GUM_ARG_ADDRESS, GUM_ADDRESS (block),
      GUM_ARG_REGISTER, GUM_REG_XBX);
}

static void
gum_exec_block_invoke_call_probes (GumExecBlock * block,
                                   GumCpuContext * cpu_context)
{
  GumStalker * stalker = block->ctx->stalker;
  const gpointer target_address = block->real_start;
  GumCallProbe ** probes_copy;
  guint num_probes, i;
  gpointer * return_address_slot;
  GumCallDetails d;

  probes_copy = NULL;
  num_probes = 0;
  {
    GPtrArray * probes;

    gum_spinlock_acquire (&stalker->probe_lock);

    probes =
        g_hash_table_lookup (stalker->probe_array_by_address, target_address);
    if (probes != NULL)
    {
      num_probes = probes->len;
      probes_copy = g_newa (GumCallProbe *, num_probes);
      for (i = 0; i != num_probes; i++)
      {
        probes_copy[i] = gum_call_probe_ref (g_ptr_array_index (probes, i));
      }
    }

    gum_spinlock_release (&stalker->probe_lock);
  }
  if (num_probes == 0)
    return;

  return_address_slot = GSIZE_TO_POINTER (GUM_CPU_CONTEXT_XSP (cpu_context));

  d.target_address = target_address;
  d.return_address = *return_address_slot;
  d.stack_data = return_address_slot;
  d.cpu_context = cpu_context;

  GUM_CPU_CONTEXT_XIP (cpu_context) = GPOINTER_TO_SIZE (target_address);

  for (i = 0; i != num_probes; i++)
  {
    GumCallProbe * probe = probes_copy[i];

    probe->callback (&d, probe->user_data);

    gum_call_probe_unref (probe);
  }
}

static gpointer
gum_exec_block_write_inline_data (GumX86Writer * cw,
                                  gconstpointer data,
                                  gsize size,
                                  GumAddress * address)
{
  gpointer location;
  gconstpointer after_data = cw->code + 1;

  while (gum_x86_writer_offset (cw) < GUM_INVALIDATE_TRAMPOLINE_SIZE)
  {
    gum_x86_writer_put_nop (cw);
  }

  if (GUM_IS_WITHIN_UINT8_RANGE (size))
    gum_x86_writer_put_jmp_short_label (cw, after_data);
  else
    gum_x86_writer_put_jmp_near_label (cw, after_data);

  location = gum_x86_writer_cur (cw);
  if (address != NULL)
    *address = cw->pc;
  gum_x86_writer_put_bytes (cw, data, size);

  gum_x86_writer_put_label (cw, after_data);

  return location;
}

static void
gum_exec_block_open_prolog (GumExecBlock * block,
                            GumPrologType type,
                            GumGeneratorContext * gc)
{
  if (gc->opened_prolog >= type)
    return;

  /* We don't want to handle this case for performance reasons */
  g_assert (gc->opened_prolog == GUM_PROLOG_NONE);

  gc->opened_prolog = type;
  gc->accumulated_stack_delta = 0;

  gum_exec_ctx_write_prolog (block->ctx, type, gc->code_writer);
}

static void
gum_exec_block_close_prolog (GumExecBlock * block,
                             GumGeneratorContext * gc)
{
  if (gc->opened_prolog == GUM_PROLOG_NONE)
    return;

  gum_exec_ctx_write_epilog (block->ctx, gc->opened_prolog, gc->code_writer);

  gc->accumulated_stack_delta = 0;
  gc->opened_prolog = GUM_PROLOG_NONE;
}

static GumCodeSlab *
gum_code_slab_new (GumExecCtx * ctx)
{
  GumCodeSlab * slab;
  GumStalker * stalker = ctx->stalker;
  const gsize slab_size = stalker->code_slab_size_dynamic;
  GumAddressSpec spec;

  gum_exec_ctx_compute_code_address_spec (ctx, slab_size, &spec);

  slab = gum_memory_allocate_near (&spec, slab_size, stalker->page_size,
      stalker->is_rwx_supported ? GUM_PAGE_RWX : GUM_PAGE_RW);

  gum_code_slab_init (slab, slab_size, stalker->page_size);

  return slab;
}

static void
gum_code_slab_free (GumCodeSlab * code_slab)
{
  gum_slab_free (&code_slab->slab);
}

static void
gum_code_slab_init (GumCodeSlab * code_slab,
                    gsize slab_size,
                    gsize page_size)
{
  /*
   * We don't want to thaw and freeze the header just to update the offset,
   * so we trade a little memory for speed.
   */
  const gsize header_size = GUM_ALIGN_SIZE (sizeof (GumCodeSlab), page_size);

  gum_slab_init (&code_slab->slab, slab_size, header_size);

  code_slab->invalidator = NULL;
}

static GumDataSlab *
gum_data_slab_new (GumExecCtx * ctx)
{
  GumDataSlab * slab;
  GumStalker * stalker = ctx->stalker;
  const gsize slab_size = stalker->data_slab_size_dynamic;
  GumAddressSpec spec;

  gum_exec_ctx_compute_data_address_spec (ctx, slab_size, &spec);

  slab = gum_memory_allocate_near (&spec, slab_size, stalker->page_size,
      GUM_PAGE_RW);

  gum_data_slab_init (slab, slab_size);

  return slab;
}

static void
gum_data_slab_free (GumDataSlab * data_slab)
{
  gum_slab_free (&data_slab->slab);
}

static void
gum_data_slab_init (GumDataSlab * data_slab,
                    gsize slab_size)
{
  GumSlab * slab = &data_slab->slab;
  const gsize header_size = sizeof (GumDataSlab);

  gum_slab_init (slab, slab_size, header_size);
}

static void
gum_scratch_slab_init (GumCodeSlab * scratch_slab,
                       gsize slab_size)
{
  const gsize header_size = sizeof (GumCodeSlab);

  gum_slab_init (&scratch_slab->slab, slab_size, header_size);

  scratch_slab->invalidator = NULL;
}

static void
gum_slab_free (GumSlab * slab)
{
  const gsize header_size = slab->data - (guint8 *) slab;

  gum_memory_free (slab, header_size + slab->size);
}

static void
gum_slab_init (GumSlab * slab,
               gsize slab_size,
               gsize header_size)
{
  slab->data = (guint8 *) slab + header_size;
  slab->offset = 0;
  slab->size = slab_size - header_size;
  slab->next = NULL;
}

static gsize
gum_slab_available (GumSlab * self)
{
  return self->size - self->offset;
}

static gpointer
gum_slab_start (GumSlab * self)
{
  return self->data;
}

static gpointer
gum_slab_end (GumSlab * self)
{
  return self->data + self->size;
}

static gpointer
gum_slab_cursor (GumSlab * self)
{
  return self->data + self->offset;
}

static gpointer
gum_slab_reserve (GumSlab * self,
                  gsize size)
{
  gpointer cursor;

  cursor = gum_slab_try_reserve (self, size);
  g_assert (cursor != NULL);

  return cursor;
}

static gpointer
gum_slab_try_reserve (GumSlab * self,
                      gsize size)
{
  gpointer cursor;

  if (gum_slab_available (self) < size)
    return NULL;

  cursor = gum_slab_cursor (self);
  self->offset += size;

  return cursor;
}

static void
gum_write_segment_prefix (uint8_t segment,
                          GumX86Writer * cw)
{
  switch (segment)
  {
    case X86_REG_INVALID: break;

    case X86_REG_CS: gum_x86_writer_put_u8 (cw, 0x2e); break;
    case X86_REG_SS: gum_x86_writer_put_u8 (cw, 0x36); break;
    case X86_REG_DS: gum_x86_writer_put_u8 (cw, 0x3e); break;
    case X86_REG_ES: gum_x86_writer_put_u8 (cw, 0x26); break;
    case X86_REG_FS: gum_x86_writer_put_u8 (cw, 0x64); break;
    case X86_REG_GS: gum_x86_writer_put_u8 (cw, 0x65); break;

    default:
      g_assert_not_reached ();
      break;
  }
}

static GumCpuReg
gum_cpu_meta_reg_from_real_reg (GumCpuReg reg)
{
  if (reg >= GUM_REG_EAX && reg <= GUM_REG_EDI)
    return (GumCpuReg) (GUM_REG_XAX + reg - GUM_REG_EAX);
  else if (reg >= GUM_REG_RAX && reg <= GUM_REG_RDI)
    return (GumCpuReg) (GUM_REG_XAX + reg - GUM_REG_RAX);
#if GLIB_SIZEOF_VOID_P == 8
  else if (reg >= GUM_REG_R8D && reg <= GUM_REG_R15D)
    return reg;
  else if (reg >= GUM_REG_R8 && reg <= GUM_REG_R15)
    return reg;
#endif
  else if (reg == GUM_REG_RIP)
    return GUM_REG_XIP;
  else if (reg != GUM_REG_NONE)
    g_assert_not_reached ();

  return GUM_REG_NONE;
}

static GumCpuReg
gum_cpu_reg_from_capstone (x86_reg reg)
{
  switch (reg)
  {
    case X86_REG_EAX: return GUM_REG_EAX;
    case X86_REG_ECX: return GUM_REG_ECX;
    case X86_REG_EDX: return GUM_REG_EDX;
    case X86_REG_EBX: return GUM_REG_EBX;
    case X86_REG_ESP: return GUM_REG_ESP;
    case X86_REG_EBP: return GUM_REG_EBP;
    case X86_REG_ESI: return GUM_REG_ESI;
    case X86_REG_EDI: return GUM_REG_EDI;
    case X86_REG_R8D: return GUM_REG_R8D;
    case X86_REG_R9D: return GUM_REG_R9D;
    case X86_REG_R10D: return GUM_REG_R10D;
    case X86_REG_R11D: return GUM_REG_R11D;
    case X86_REG_R12D: return GUM_REG_R12D;
    case X86_REG_R13D: return GUM_REG_R13D;
    case X86_REG_R14D: return GUM_REG_R14D;
    case X86_REG_R15D: return GUM_REG_R15D;
    case X86_REG_EIP: return GUM_REG_EIP;

    case X86_REG_RAX: return GUM_REG_RAX;
    case X86_REG_RCX: return GUM_REG_RCX;
    case X86_REG_RDX: return GUM_REG_RDX;
    case X86_REG_RBX: return GUM_REG_RBX;
    case X86_REG_RSP: return GUM_REG_RSP;
    case X86_REG_RBP: return GUM_REG_RBP;
    case X86_REG_RSI: return GUM_REG_RSI;
    case X86_REG_RDI: return GUM_REG_RDI;
    case X86_REG_R8: return GUM_REG_R8;
    case X86_REG_R9: return GUM_REG_R9;
    case X86_REG_R10: return GUM_REG_R10;
    case X86_REG_R11: return GUM_REG_R11;
    case X86_REG_R12: return GUM_REG_R12;
    case X86_REG_R13: return GUM_REG_R13;
    case X86_REG_R14: return GUM_REG_R14;
    case X86_REG_R15: return GUM_REG_R15;
    case X86_REG_RIP: return GUM_REG_RIP;

    default:
      return GUM_REG_NONE;
  }
}

static x86_insn
gum_negate_jcc (x86_insn instruction_id)
{
  switch (instruction_id)
  {
    case X86_INS_JA:
      return X86_INS_JBE;
    case X86_INS_JAE:
      return X86_INS_JB;
    case X86_INS_JB:
      return X86_INS_JAE;
    case X86_INS_JBE:
      return X86_INS_JA;
    case X86_INS_JE:
      return X86_INS_JNE;
    case X86_INS_JG:
      return X86_INS_JLE;
    case X86_INS_JGE:
      return X86_INS_JL;
    case X86_INS_JL:
      return X86_INS_JGE;
    case X86_INS_JLE:
      return X86_INS_JG;
    case X86_INS_JNE:
      return X86_INS_JE;
    case X86_INS_JNO:
      return X86_INS_JO;
    case X86_INS_JNP:
      return X86_INS_JP;
    case X86_INS_JNS:
      return X86_INS_JS;
    case X86_INS_JO:
      return X86_INS_JNO;
    case X86_INS_JP:
      return X86_INS_JNP;
    case X86_INS_JS:
    default:
      return X86_INS_JNS;
  }
}

#ifdef HAVE_WINDOWS

static gboolean
gum_stalker_on_exception (GumExceptionDetails * details,
                          gpointer user_data)
{
  GumStalker * self = GUM_STALKER (user_data);
  GumCpuContext * cpu_context = &details->context;
  CONTEXT * tc = details->native_context;
  GumExecCtx * candidate_ctx;

  if (details->type != GUM_EXCEPTION_SINGLE_STEP)
    return FALSE;

  candidate_ctx =
      gum_stalker_find_exec_ctx_by_thread_id (self, details->thread_id);
  if (candidate_ctx != NULL &&
      GUM_CPU_CONTEXT_XIP (cpu_context) == candidate_ctx->previous_pc)
  {
    GumExecCtx * pending_ctx = candidate_ctx;

    tc->Dr0 = pending_ctx->previous_dr0;
    tc->Dr7 = pending_ctx->previous_dr7;

    pending_ctx->previous_pc = 0;

    GUM_CPU_CONTEXT_XIP (cpu_context) = pending_ctx->infect_body;

    return TRUE;
  }

# if GLIB_SIZEOF_VOID_P == 8
  return FALSE;
# else
  {
    GumExecCtx * ctx;

    ctx = gum_stalker_get_exec_ctx (self);
    if (ctx == NULL)
      return FALSE;

    switch (ctx->mode)
    {
      case GUM_EXEC_CTX_NORMAL:
      case GUM_EXEC_CTX_SINGLE_STEPPING_ON_CALL:
      {
        DWORD instruction_after_call_here;
        DWORD instruction_after_call_above_us;

        ctx->previous_dr0 = tc->Dr0;
        ctx->previous_dr1 = tc->Dr1;
        ctx->previous_dr2 = tc->Dr2;
        ctx->previous_dr7 = tc->Dr7;

        tc->Dr7 = 0x00000700;

        instruction_after_call_here = cpu_context->eip +
            gum_x86_reader_insn_length ((guint8 *) cpu_context->eip);
        tc->Dr0 = instruction_after_call_here;
        gum_enable_hardware_breakpoint (&tc->Dr7, 0);

        tc->Dr1 = (DWORD) self->ki_user_callback_dispatcher_impl;
        gum_enable_hardware_breakpoint (&tc->Dr7, 1);

        instruction_after_call_above_us =
            (DWORD) gum_find_system_call_above_us (self,
                (gpointer *) cpu_context->esp);
        if (instruction_after_call_above_us != 0)
        {
          tc->Dr2 = instruction_after_call_above_us;
          gum_enable_hardware_breakpoint (&tc->Dr7, 2);
        }

        ctx->mode = GUM_EXEC_CTX_SINGLE_STEPPING_THROUGH_CALL;

        break;
      }
      case GUM_EXEC_CTX_SINGLE_STEPPING_THROUGH_CALL:
      {
        tc->Dr0 = ctx->previous_dr0;
        tc->Dr1 = ctx->previous_dr1;
        tc->Dr2 = ctx->previous_dr2;
        tc->Dr7 = ctx->previous_dr7;

        gum_exec_ctx_switch_block (ctx, GSIZE_TO_POINTER (cpu_context->eip));
        cpu_context->eip = (DWORD) ctx->resume_at;

        ctx->mode = GUM_EXEC_CTX_NORMAL;

        break;
      }
      default:
        g_assert_not_reached ();
    }

    return TRUE;
  }
#endif
}

static void
gum_enable_hardware_breakpoint (GumNativeRegisterValue * dr7_reg,
                                guint index)
{
  /* Set both RWn and LENn to 00. */
  *dr7_reg &= ~((GumNativeRegisterValue) 0xf << (16 + (2 * index)));

  /* Set LE bit. */
  *dr7_reg |= (GumNativeRegisterValue) (1 << (2 * index));
}

# if GLIB_SIZEOF_VOID_P == 4

static void
gum_collect_export (GArray * impls,
                    const TCHAR * module_name,
                    const gchar * export_name)
{
  HMODULE module_handle;

  module_handle = GetModuleHandle (module_name);
  if (module_handle == NULL)
    return;

  gum_collect_export_by_handle (impls, module_handle, export_name);
}

static void
gum_collect_export_by_handle (GArray * impls,
                              HMODULE module_handle,
                              const gchar * export_name)
{
  gsize impl;

  impl = GPOINTER_TO_SIZE (GetProcAddress (module_handle, export_name));
  if (impl == 0)
    return;

  g_array_append_val (impls, impl);
}

static gpointer
gum_find_system_call_above_us (GumStalker * stalker,
                               gpointer * start_esp)
{
  gpointer * top_esp, * cur_esp;
  guint8 call_fs_c0_code[] = { 0x64, 0xff, 0x15, 0xc0, 0x00, 0x00, 0x00 };
  guint8 call_ebp_8_code[] = { 0xff, 0x55, 0x08 };
  guint8 * minimum_address, * maximum_address;

  __asm
  {
    mov eax, fs:[4];
    mov [top_esp], eax;
  }

  if ((guint) ABS (top_esp - start_esp) > stalker->page_size)
  {
    top_esp = (gpointer *) ((GPOINTER_TO_SIZE (start_esp) +
        (stalker->page_size - 1)) & ~(stalker->page_size - 1));
  }

  /* These boundaries are quite artificial... */
  minimum_address = (guint8 *) stalker->user32_start + sizeof (call_fs_c0_code);
  maximum_address = (guint8 *) stalker->user32_end - 1;

  for (cur_esp = start_esp + 1; cur_esp < top_esp; cur_esp++)
  {
    guint8 * address = (guint8 *) *cur_esp;

    if (address >= minimum_address && address <= maximum_address)
    {
      if (memcmp (address - sizeof (call_fs_c0_code), call_fs_c0_code,
          sizeof (call_fs_c0_code)) == 0
          || memcmp (address - sizeof (call_ebp_8_code), call_ebp_8_code,
          sizeof (call_ebp_8_code)) == 0)
      {
        return address;
      }
    }
  }

  return NULL;
}

# endif

#endif

static gpointer
gum_find_thread_exit_implementation (void)
{
#ifdef HAVE_DARWIN
  GumAddress result = 0;
  const gchar * pthread_path = "/usr/lib/system/libsystem_pthread.dylib";
  GumMemoryRange range;
  GumMatchPattern * pattern;

  range.base_address = gum_module_find_base_address (pthread_path);
  range.size = 128 * 1024;

  pattern = gum_match_pattern_new_from_string (
#if GLIB_SIZEOF_VOID_P == 8
     /*
      * Verified on macOS:
      * - 10.14.6
      * - 10.15.6
      * - 11.0 Beta 3
      */
      "55 "            /* push rbp                       */
      "48 89 e5 "      /* mov rbp, rsp                   */
      "41 57 "         /* push r15                       */
      "41 56 "         /* push r14                       */
      "53 "            /* push rbx                       */
      "50 "            /* push rax                       */
      "49 89 f6 "      /* mov r14, rsi                   */
      "49 89 ff"       /* mov r15, rdi                   */
      "bf 01 00 00 00" /* mov edi, 0x1                   */
#else
      /*
       * Verified on macOS:
       * - 10.14.6
       */
      "55 "            /* push ebp                       */
      "89 e5 "         /* mov ebp, esp                   */
      "53 "            /* push ebx                       */
      "57 "            /* push edi                       */
      "56 "            /* push esi                       */
      "83 ec 0c "      /* sub esp, 0xc                   */
      "89 d6 "         /* mov esi, edx                   */
      "89 cf"          /* mov edi, ecx                   */
#endif
  );

  gum_memory_scan (&range, pattern, gum_store_thread_exit_match, &result);

  gum_match_pattern_free (pattern);

  /* Non-public symbols are all <redacted> on iOS. */
#ifndef HAVE_IOS
  if (result == 0)
    result = gum_module_find_symbol_by_name (pthread_path, "_pthread_exit");
#endif

  return GSIZE_TO_POINTER (result);
#else
  return NULL;
#endif
}

#ifdef HAVE_DARWIN

static gboolean
gum_store_thread_exit_match (GumAddress address,
                             gsize size,
                             gpointer user_data)
{
  GumAddress * result = user_data;

  *result = address;

  return FALSE;
}

#endif
