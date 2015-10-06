#include <stdio.h>

#include "lily_alloc.h"
#include "lily_type_system.h"


# define ENSURE_TYPE_STACK(new_size) \
if (new_size >= ts->max) \
    grow_types(ts);

/* If this is set, then check_generic will not attempt to do solving if it encounters a
   generic. It will instead rely on direct equality.
   When calling a raw matcher, this must be supplied, as the default is to solve for
   generics. */
#define T_DONT_SOLVE 0x1

/* If this is set, then consider two types to be equivalent to each other if the right
   side provides more than the left side (something more derived).
   As of right now, only function returns make use of this. */
#define T_COVARIANT 0x2

/* If this is set, then consider two types to be equivalent to each other if the right
   side provides LESS than the left side (something less derived).
   As of right now, only function inputs use this. */
#define T_CONTRAVARIANT 0x4

lily_type_system *lily_new_type_system(lily_type_maker *tm)
{
    lily_type_system *ts = lily_malloc(sizeof(lily_type_system));
    lily_type **types = lily_malloc(4 * sizeof(lily_type *));

    ts->tm = tm;
    ts->types = types;
    ts->pos = 0;
    ts->max = 4;
    ts->max_seen = 0;
    ts->ceiling = 0;

    return ts;
}

void lily_free_type_system(lily_type_system *ts)
{
    if (ts)
        lily_free(ts->types);

    lily_free(ts);
}

static void grow_types(lily_type_system *ts)
{
    ts->max *= 2;
    ts->types = lily_realloc(ts->types,
            sizeof(lily_type *) * ts->max);;
}

static lily_type *deep_type_build(lily_type_system *ts, int generic_index,
        lily_type *type)
{
    lily_type *ret = type;

    if (type == NULL)
        /* functions use NULL to indicate they don't return a value. */
        ret = NULL;
    else if (type->subtypes != NULL) {
        int i, save_start;
        lily_type **subtypes = type->subtypes;
        ENSURE_TYPE_STACK(ts->pos + type->subtype_count)

        save_start = ts->pos;

        for (i = 0;i < type->subtype_count;i++) {
            lily_type *inner_type = deep_type_build(ts, generic_index,
                    subtypes[i]);
            ts->types[ts->pos] = inner_type;
            ts->pos++;
        }

        ret = lily_tm_raw_make(ts->tm, type->flags, type->cls, ts->types,
                save_start, i);

        ts->pos -= i;
    }
    else if (type->cls->id == SYM_CLASS_GENERIC) {
        ret = ts->types[generic_index + type->generic_pos];
        /* Sometimes, a generic is wanted that was never filled in. In such a
           case, use 'any' because it is the most accepting of values. */
        if (ret == NULL) {
            ret = ts->any_class_type;
            /* This allows lambdas to determine that a given generic was not
               resolved (and prevent it). */
            ts->types[generic_index + type->generic_pos] = ret;
        }
    }
    return ret;
}

void lily_ts_pull_generics(lily_type_system *ts, lily_type *left, lily_type *right)
{
    if (left == NULL)
        return;
    else if (left->flags & TYPE_IS_UNRESOLVED) {
        if (left->cls->id == SYM_CLASS_GENERIC)
            ts->types[ts->pos + left->generic_pos] = right;
        else if (left->subtype_count) {
            int i;
            for (i = 0;i < left->subtype_count;i++) {
                lily_type *left_sub = left->subtypes[i];
                lily_type *right_sub = right->subtypes[i];

                lily_ts_pull_generics(ts, left_sub, right_sub);
            }
        }
    }
}

static int check_raw(lily_type_system *, lily_type *, lily_type *, int);

static int check_generic(lily_type_system *ts, lily_type *left,
        lily_type *right, int flags)
{
    int ret;
    if (flags & T_DONT_SOLVE)
        ret = (left == right);
    else {
        int generic_pos = ts->pos + left->generic_pos;
        lily_type *cmp_type = ts->types[generic_pos];
        ret = 1;
        if (cmp_type == NULL)
            ts->types[generic_pos] = right;
        else if (cmp_type == right)
            ;
        else if (flags & (T_COVARIANT | T_CONTRAVARIANT))
            ret = check_raw(ts, cmp_type, right, flags | T_DONT_SOLVE);
        else
            ret = 0;
    }

    return ret;
}

static int check_enum(lily_type_system *ts, lily_type *left, lily_type *right,
        int flags)
{
    int ret = 1;

    if (right->cls->variant_type->subtype_count != 0) {
        /* Erase the variance of the caller, since it doesn't apply to the subtypes of
           this class. check_misc explains why this is important in more detail. */
        flags &= T_DONT_SOLVE;

        /* I think this is best explained as an example:
            'enum Option[A, B] { Some(A) None }'
            In this case, the variant type of Some is defined as:
            'function (A => Some[A])'
            This pulls the 'Some[A]'. */
        lily_type *variant_output = right->cls->variant_type->subtypes[0];
        int i;
        /* The result is an Option[A, B], but Some only has A. Match up
            generics that are available, to proper positions in the
            parent. If any fail, then stop. */
        for (i = 0;i < variant_output->subtype_count;i++) {
            int pos = variant_output->subtypes[i]->generic_pos;
            ret = check_raw(ts, left->subtypes[pos], right->subtypes[i], flags);
            if (ret == 0)
                break;
        }
    }

    return ret;
}

static int check_function(lily_type_system *ts, lily_type *left,
        lily_type *right, int flags)
{
    int ret = 1;
    flags &= T_DONT_SOLVE;

    /* Remember that [0] is the return type, and always exists. */
    if (check_raw(ts, left->subtypes[0], right->subtypes[0], flags | T_COVARIANT) == 0)
        ret = 0;

    if (left->subtype_count > right->subtype_count)
        ret = 0;

    if (ret) {
        flags |= T_CONTRAVARIANT;
        int i;
        for (i = 1;i < left->subtype_count;i++) {
            lily_type *left_type = left->subtypes[i];
            lily_type *right_type = right->subtypes[i];

            if (right_type->cls->id == SYM_CLASS_OPTARG &&
                left_type->cls->id != SYM_CLASS_OPTARG) {
                right_type = right_type->subtypes[0];
            }

            if (check_raw(ts, left_type, right_type, flags) == 0) {
                ret = 0;
                break;
            }
        }
    }

    return ret;
}

static int invariant_check(lily_type *left, lily_type *right, int *num_subtypes)
{
    int ret = left->cls == right->cls;
    *num_subtypes = left->subtype_count;

    return ret;
}

static int covariant_check(lily_type *left, lily_type *right, int *num_subtypes)
{
    int ret = lily_class_greater_eq(left->cls, right->cls);
    *num_subtypes = left->subtype_count;

    return ret;
}

static int contravariant_check(lily_type *left, lily_type *right, int *num_subtypes)
{
    int ret;
    if (left->cls == right->cls)
        ret = (left->subtype_count == right->subtype_count);
    else
        ret = lily_class_greater_eq(right->cls, left->cls);

    *num_subtypes = right->subtype_count;
    return ret;
}

static int check_misc(lily_type_system *ts, lily_type *left, lily_type *right,
        int flags)
{
    int ret;
    int num_subtypes;

    if (flags & T_COVARIANT)
        ret = covariant_check(left, right, &num_subtypes);
    else if (flags & T_CONTRAVARIANT)
        ret = contravariant_check(left, right, &num_subtypes);
    else
        ret = invariant_check(left, right, &num_subtypes);

    if (ret && num_subtypes) {
        /* This is really important. The caller's variance extends up to this class, but
           not into it. The caller may want contravariant matching, but the class may
           have its generics listed as being invariant.
           Proof:

           ```
           class Point() { ... }
           class Point3D() > Point() { ... }
           define f(in: list[Point3D]) { ... }
           define g(in: list[Point]) {
               in.append(Point::new())
           }

           # Type: list[Point3D]
           var v = [Point3D::new()]
           # After this, v[1] has type Point, but should be at least Point3D.
           g(v)

           ``` */
        flags &= T_DONT_SOLVE;

        ret = 1;

        lily_type **left_subtypes = left->subtypes;
        lily_type **right_subtypes = right->subtypes;
        int i;
        for (i = 0;i < num_subtypes;i++) {
            lily_type *left_entry = left_subtypes[i];
            lily_type *right_entry = right_subtypes[i];
            if (left_entry != right_entry &&
                check_raw(ts, left_entry, right_entry, flags) == 0) {
                ret = 0;
                break;
            }
        }
    }

    return ret;
}

static int check_raw(lily_type_system *ts, lily_type *left, lily_type *right, int flags)
{
    int ret = 0;

    if (left == NULL || right == NULL)
        ret = (left == right);
    else if (left->cls->id == SYM_CLASS_GENERIC)
        ret = check_generic(ts, left, right, flags);
    else if (left->cls->flags & CLS_IS_ENUM &&
             right->cls->flags & CLS_IS_VARIANT &&
             right->cls->parent == left->cls)
        ret = check_enum(ts, left, right, flags);
    else if (left->cls->id == SYM_CLASS_FUNCTION &&
             right->cls->id == SYM_CLASS_FUNCTION)
        ret = check_function(ts, left, right, flags);
    else
        ret = check_misc(ts, left, right, flags);

    return ret;
}

int lily_ts_check(lily_type_system *ts, lily_type *left, lily_type *right)
{
    return check_raw(ts, left, right, 0);
}

int lily_ts_type_greater_eq(lily_type_system *ts, lily_type *left, lily_type *right)
{
    return check_raw(ts, left, right, T_DONT_SOLVE | T_COVARIANT);
}

inline lily_type *lily_ts_easy_resolve(lily_type_system *ts, lily_type *t)
{
    return ts->types[ts->pos + t->generic_pos];
}

lily_type *lily_ts_resolve(lily_type_system *ts, lily_type *type)
{
    int save_generic_index = ts->pos;

    ts->pos += ts->ceiling;
    lily_type *ret = deep_type_build(ts, save_generic_index, type);
    ts->pos -= ts->ceiling;

    return ret;
}

lily_type *lily_ts_resolve_by_second(lily_type_system *ts, lily_type *first,
        lily_type *second)
{
    int stack_start = ts->pos + ts->ceiling + 1;
    int save_ssp = ts->pos;

    ENSURE_TYPE_STACK(stack_start + first->subtype_count)

    int i;
    for (i = 0;i < first->subtype_count;i++)
        ts->types[stack_start + i] = first->subtypes[i];

    ts->pos = stack_start;
    lily_type *result_type = lily_ts_resolve(ts, second);
    ts->pos = save_ssp;

    return result_type;
}

void lily_ts_resolve_as_variant_by_enum(lily_type_system *ts,
        lily_type *call_result, lily_type *enum_type)
{
    lily_type *variant_type = call_result->cls->variant_type->subtypes[0];
    int max = call_result->subtype_count;
    int i;

    for (i = 0;i < max;i++) {
        int pos = variant_type->subtypes[0]->generic_pos;
        ts->types[ts->pos + pos] = enum_type->subtypes[pos];
    }
}

void lily_ts_resolve_as_self(lily_type_system *ts, lily_type *generic_iter)
{
    int i, stop;

    stop = ts->pos + ts->ceiling;
    for (i = ts->pos;i < stop;i++, generic_iter = generic_iter->next) {
        if (ts->types[i] == NULL)
            ts->types[i] = generic_iter;
    }
}

int lily_ts_raise_ceiling(lily_type_system *ts)
{
    int old_ceiling = ts->ceiling;
    int i;

    /* This probably seems complicated. Let's break it down:
       * ts->pos + ts->ceiling: This is where types are currently being sent.
         It's important to not damage anything in here.
       * ts->max_seen * 2: This ensures that there will be enough space beyond
         the ceiling to do intermediate calculations. */
    ENSURE_TYPE_STACK(ts->pos + ts->ceiling + (ts->max_seen * 2));
    ts->pos += ts->ceiling;
    ts->ceiling = ts->max_seen;
    for (i = 0;i < ts->max_seen;i++)
        ts->types[ts->pos + i] = NULL;

    return old_ceiling;
}

inline void lily_ts_lower_ceiling(lily_type_system *ts, int old_ceiling)
{
    ts->pos -= old_ceiling;
    ts->ceiling = old_ceiling;
}

inline void lily_ts_set_ceiling_type(lily_type_system *ts, lily_type *type,
        int pos)
{
    ts->types[ts->pos + ts->ceiling + 1 + pos] = type;
}

inline lily_type *lily_ts_get_ceiling_type(lily_type_system *ts, int pos)
{
    return ts->types[ts->pos + ts->ceiling + 1 + pos];
}

int lily_ts_enum_membership_check(lily_type_system *ts, lily_type *enum_type,
        lily_type *variant_type)
{
    int ret;
    if (variant_type->cls->parent == enum_type->cls)
        ret = check_enum(ts, enum_type, variant_type, T_DONT_SOLVE);
    else
        ret = 0;

    return ret;
}

int lily_ts_count_unresolved(lily_type_system *ts)
{
    int count = 0, top = ts->pos + ts->ceiling;
    int i;
    for (i = ts->pos;i < top;i++) {
        if (ts->types[i] == NULL)
            count++;
    }

    return count;
}

void lily_ts_generics_seen(lily_type_system *ts, int amount)
{
    if (amount > ts->max_seen)
        ts->max_seen = amount;
}

int lily_class_greater_eq(lily_class *left, lily_class *right)
{
    int ret = 0;
    if (left != right) {
        while (right != NULL) {
            right = right->parent;
            if (right == left) {
                ret = 1;
                break;
            }
        }
    }
    else
        ret = 1;

    return ret;
}
