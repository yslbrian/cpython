
/* Thread module */
/* Interface to Sjoerd's portable C thread library */

#include "Python.h"
#include "pycore_interp.h"        // _PyInterpreterState.threads.count
#include "pycore_moduleobject.h"  // _PyModule_GetState()
#include "pycore_modsupport.h"    // _PyArg_NoKeywords()
#include "pycore_pylifecycle.h"
#include "pycore_pystate.h"       // _PyThreadState_SetCurrent()
#include "pycore_sysmodule.h"     // _PySys_GetAttr()
#include "pycore_time.h"          // _PyTime_FromSeconds()
#include "pycore_weakref.h"       // _PyWeakref_GET_REF()

#include <stdbool.h>
#include <stddef.h>               // offsetof()
#ifdef HAVE_SIGNAL_H
#  include <signal.h>             // SIGINT
#endif

// ThreadError is just an alias to PyExc_RuntimeError
#define ThreadError PyExc_RuntimeError


// Forward declarations
static struct PyModuleDef thread_module;

// Module state
typedef struct {
    PyTypeObject *excepthook_type;
    PyTypeObject *lock_type;
    PyTypeObject *local_type;
    PyTypeObject *local_dummy_type;
    PyTypeObject *thread_handle_type;
} thread_module_state;

static inline thread_module_state*
get_thread_state(PyObject *module)
{
    void *state = _PyModule_GetState(module);
    assert(state != NULL);
    return (thread_module_state *)state;
}

// _ThreadHandle type

typedef struct {
    PyObject_HEAD
    struct llist_node node;  // linked list node (see _pythread_runtime_state)
    PyThread_ident_t ident;
    PyThread_handle_t handle;
    char joinable;
} ThreadHandleObject;

static ThreadHandleObject*
new_thread_handle(thread_module_state* state)
{
    ThreadHandleObject* self = PyObject_New(ThreadHandleObject, state->thread_handle_type);
    if (self == NULL) {
        return NULL;
    }
    self->ident = 0;
    self->handle = 0;
    self->joinable = 0;

    HEAD_LOCK(&_PyRuntime);
    llist_insert_tail(&_PyRuntime.threads.handles, &self->node);
    HEAD_UNLOCK(&_PyRuntime);

    return self;
}

static void
ThreadHandle_dealloc(ThreadHandleObject *self)
{
    PyObject *tp = (PyObject *) Py_TYPE(self);

    // Remove ourself from the global list of handles
    HEAD_LOCK(&_PyRuntime);
    if (self->node.next != NULL) {
        llist_remove(&self->node);
    }
    HEAD_UNLOCK(&_PyRuntime);

    if (self->joinable) {
        int ret = PyThread_detach_thread(self->handle);
        if (ret) {
            PyErr_SetString(ThreadError, "Failed detaching thread");
            PyErr_WriteUnraisable(tp);
        }
    }
    PyObject_Free(self);
    Py_DECREF(tp);
}

void
_PyThread_AfterFork(struct _pythread_runtime_state *state)
{
    // gh-115035: We mark ThreadHandles as not joinable early in the child's
    // after-fork handler. We do this before calling any Python code to ensure
    // that it happens before any ThreadHandles are deallocated, such as by a
    // GC cycle.
    PyThread_ident_t current = PyThread_get_thread_ident_ex();

    struct llist_node *node;
    llist_for_each_safe(node, &state->handles) {
        ThreadHandleObject *hobj = llist_data(node, ThreadHandleObject, node);
        if (hobj->ident == current) {
            continue;
        }

        // Disallow calls to detach() and join() as they could crash.
        hobj->joinable = 0;
        llist_remove(node);
    }
}

static PyObject *
ThreadHandle_repr(ThreadHandleObject *self)
{
    return PyUnicode_FromFormat("<%s object: ident=%" PY_FORMAT_THREAD_IDENT_T ">",
                                Py_TYPE(self)->tp_name, self->ident);
}

static PyObject *
ThreadHandle_get_ident(ThreadHandleObject *self, void *ignored)
{
    return PyLong_FromUnsignedLongLong(self->ident);
}


static PyObject *
ThreadHandle_detach(ThreadHandleObject *self, void* ignored)
{
    if (!self->joinable) {
        PyErr_SetString(PyExc_ValueError,
                        "the thread is not joinable and thus cannot be detached");
        return NULL;
    }
    self->joinable = 0;
    // This is typically short so no need to release the GIL
    int ret = PyThread_detach_thread(self->handle);
    if (ret) {
        PyErr_SetString(ThreadError, "Failed detaching thread");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyObject *
ThreadHandle_join(ThreadHandleObject *self, void* ignored)
{
    if (!self->joinable) {
        PyErr_SetString(PyExc_ValueError, "the thread is not joinable");
        return NULL;
    }
    if (self->ident == PyThread_get_thread_ident_ex()) {
        // PyThread_join_thread() would deadlock or error out.
        PyErr_SetString(ThreadError, "Cannot join current thread");
        return NULL;
    }
    // Before actually joining, we must first mark the thread as non-joinable,
    // as joining several times simultaneously or sequentially is undefined behavior.
    self->joinable = 0;
    int ret;
    Py_BEGIN_ALLOW_THREADS
    ret = PyThread_join_thread(self->handle);
    Py_END_ALLOW_THREADS
    if (ret) {
        PyErr_SetString(ThreadError, "Failed joining thread");
        return NULL;
    }
    Py_RETURN_NONE;
}

static PyGetSetDef ThreadHandle_getsetlist[] = {
    {"ident", (getter)ThreadHandle_get_ident, NULL, NULL},
    {0},
};

static PyMethodDef ThreadHandle_methods[] =
{
    {"detach", (PyCFunction)ThreadHandle_detach, METH_NOARGS},
    {"join", (PyCFunction)ThreadHandle_join, METH_NOARGS},
    {0, 0}
};

static PyType_Slot ThreadHandle_Type_slots[] = {
    {Py_tp_dealloc, (destructor)ThreadHandle_dealloc},
    {Py_tp_repr, (reprfunc)ThreadHandle_repr},
    {Py_tp_getset, ThreadHandle_getsetlist},
    {Py_tp_methods, ThreadHandle_methods},
    {0, 0}
};

static PyType_Spec ThreadHandle_Type_spec = {
    "_thread._ThreadHandle",
    sizeof(ThreadHandleObject),
    0,
    Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION,
    ThreadHandle_Type_slots,
};

/* Lock objects */

typedef struct {
    PyObject_HEAD
    PyThread_type_lock lock_lock;
    PyObject *in_weakreflist;
    char locked; /* for sanity checking */
} lockobject;

static int
lock_traverse(lockobject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    return 0;
}

static void
lock_dealloc(lockobject *self)
{
    PyObject_GC_UnTrack(self);
    if (self->in_weakreflist != NULL) {
        PyObject_ClearWeakRefs((PyObject *) self);
    }
    if (self->lock_lock != NULL) {
        /* Unlock the lock so it's safe to free it */
        if (self->locked)
            PyThread_release_lock(self->lock_lock);
        PyThread_free_lock(self->lock_lock);
    }
    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free((PyObject*)self);
    Py_DECREF(tp);
}

static inline PyLockStatus
acquire_timed(PyThread_type_lock lock, PyTime_t timeout)
{
    return PyThread_acquire_lock_timed_with_retries(lock, timeout);
}

static int
lock_acquire_parse_args(PyObject *args, PyObject *kwds,
                        PyTime_t *timeout)
{
    char *kwlist[] = {"blocking", "timeout", NULL};
    int blocking = 1;
    PyObject *timeout_obj = NULL;
    if (!PyArg_ParseTupleAndKeywords(args, kwds, "|pO:acquire", kwlist,
                                     &blocking, &timeout_obj))
        return -1;

    // XXX Use PyThread_ParseTimeoutArg().

    const PyTime_t unset_timeout = _PyTime_FromSeconds(-1);
    *timeout = unset_timeout;

    if (timeout_obj
        && _PyTime_FromSecondsObject(timeout,
                                     timeout_obj, _PyTime_ROUND_TIMEOUT) < 0)
        return -1;

    if (!blocking && *timeout != unset_timeout ) {
        PyErr_SetString(PyExc_ValueError,
                        "can't specify a timeout for a non-blocking call");
        return -1;
    }
    if (*timeout < 0 && *timeout != unset_timeout) {
        PyErr_SetString(PyExc_ValueError,
                        "timeout value must be a non-negative number");
        return -1;
    }
    if (!blocking)
        *timeout = 0;
    else if (*timeout != unset_timeout) {
        PyTime_t microseconds;

        microseconds = _PyTime_AsMicroseconds(*timeout, _PyTime_ROUND_TIMEOUT);
        if (microseconds > PY_TIMEOUT_MAX) {
            PyErr_SetString(PyExc_OverflowError,
                            "timeout value is too large");
            return -1;
        }
    }
    return 0;
}

static PyObject *
lock_PyThread_acquire_lock(lockobject *self, PyObject *args, PyObject *kwds)
{
    PyTime_t timeout;
    if (lock_acquire_parse_args(args, kwds, &timeout) < 0)
        return NULL;

    PyLockStatus r = acquire_timed(self->lock_lock, timeout);
    if (r == PY_LOCK_INTR) {
        return NULL;
    }

    if (r == PY_LOCK_ACQUIRED)
        self->locked = 1;
    return PyBool_FromLong(r == PY_LOCK_ACQUIRED);
}

PyDoc_STRVAR(acquire_doc,
"acquire(blocking=True, timeout=-1) -> bool\n\
(acquire_lock() is an obsolete synonym)\n\
\n\
Lock the lock.  Without argument, this blocks if the lock is already\n\
locked (even by the same thread), waiting for another thread to release\n\
the lock, and return True once the lock is acquired.\n\
With an argument, this will only block if the argument is true,\n\
and the return value reflects whether the lock is acquired.\n\
The blocking operation is interruptible.");

static PyObject *
lock_PyThread_release_lock(lockobject *self, PyObject *Py_UNUSED(ignored))
{
    /* Sanity check: the lock must be locked */
    if (!self->locked) {
        PyErr_SetString(ThreadError, "release unlocked lock");
        return NULL;
    }

    PyThread_release_lock(self->lock_lock);
    self->locked = 0;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(release_doc,
"release()\n\
(release_lock() is an obsolete synonym)\n\
\n\
Release the lock, allowing another thread that is blocked waiting for\n\
the lock to acquire the lock.  The lock must be in the locked state,\n\
but it needn't be locked by the same thread that unlocks it.");

static PyObject *
lock_locked_lock(lockobject *self, PyObject *Py_UNUSED(ignored))
{
    return PyBool_FromLong((long)self->locked);
}

PyDoc_STRVAR(locked_doc,
"locked() -> bool\n\
(locked_lock() is an obsolete synonym)\n\
\n\
Return whether the lock is in the locked state.");

static PyObject *
lock_repr(lockobject *self)
{
    return PyUnicode_FromFormat("<%s %s object at %p>",
        self->locked ? "locked" : "unlocked", Py_TYPE(self)->tp_name, self);
}

#ifdef HAVE_FORK
static PyObject *
lock__at_fork_reinit(lockobject *self, PyObject *Py_UNUSED(args))
{
    if (_PyThread_at_fork_reinit(&self->lock_lock) < 0) {
        PyErr_SetString(ThreadError, "failed to reinitialize lock at fork");
        return NULL;
    }

    self->locked = 0;

    Py_RETURN_NONE;
}
#endif  /* HAVE_FORK */

static lockobject *newlockobject(PyObject *module);

static PyObject *
lock_new(PyTypeObject *type, PyObject *args, PyObject *kwargs)
{
    // convert to AC?
    if (!_PyArg_NoKeywords("lock", kwargs)) {
        goto error;
    }
    if (!_PyArg_CheckPositional("lock", PyTuple_GET_SIZE(args), 0, 0)) {
        goto error;
    }

    PyObject *module = PyType_GetModuleByDef(type, &thread_module);
    assert(module != NULL);
    return (PyObject *)newlockobject(module);

error:
    return NULL;
}


static PyMethodDef lock_methods[] = {
    {"acquire_lock", _PyCFunction_CAST(lock_PyThread_acquire_lock),
     METH_VARARGS | METH_KEYWORDS, acquire_doc},
    {"acquire",      _PyCFunction_CAST(lock_PyThread_acquire_lock),
     METH_VARARGS | METH_KEYWORDS, acquire_doc},
    {"release_lock", (PyCFunction)lock_PyThread_release_lock,
     METH_NOARGS, release_doc},
    {"release",      (PyCFunction)lock_PyThread_release_lock,
     METH_NOARGS, release_doc},
    {"locked_lock",  (PyCFunction)lock_locked_lock,
     METH_NOARGS, locked_doc},
    {"locked",       (PyCFunction)lock_locked_lock,
     METH_NOARGS, locked_doc},
    {"__enter__",    _PyCFunction_CAST(lock_PyThread_acquire_lock),
     METH_VARARGS | METH_KEYWORDS, acquire_doc},
    {"__exit__",    (PyCFunction)lock_PyThread_release_lock,
     METH_VARARGS, release_doc},
#ifdef HAVE_FORK
    {"_at_fork_reinit",    (PyCFunction)lock__at_fork_reinit,
     METH_NOARGS, NULL},
#endif
    {NULL,           NULL}              /* sentinel */
};

PyDoc_STRVAR(lock_doc,
"A lock object is a synchronization primitive.  To create a lock,\n\
call threading.Lock().  Methods are:\n\
\n\
acquire() -- lock the lock, possibly blocking until it can be obtained\n\
release() -- unlock of the lock\n\
locked() -- test whether the lock is currently locked\n\
\n\
A lock is not owned by the thread that locked it; another thread may\n\
unlock it.  A thread attempting to lock a lock that it has already locked\n\
will block until another thread unlocks it.  Deadlocks may ensue.");

static PyMemberDef lock_type_members[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(lockobject, in_weakreflist), Py_READONLY},
    {NULL},
};

static PyType_Slot lock_type_slots[] = {
    {Py_tp_dealloc, (destructor)lock_dealloc},
    {Py_tp_repr, (reprfunc)lock_repr},
    {Py_tp_doc, (void *)lock_doc},
    {Py_tp_methods, lock_methods},
    {Py_tp_traverse, lock_traverse},
    {Py_tp_members, lock_type_members},
    {Py_tp_new, lock_new},
    {0, 0}
};

static PyType_Spec lock_type_spec = {
    .name = "_thread.lock",
    .basicsize = sizeof(lockobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_HAVE_GC |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = lock_type_slots,
};

/* Recursive lock objects */

typedef struct {
    PyObject_HEAD
    PyThread_type_lock rlock_lock;
    PyThread_ident_t rlock_owner;
    unsigned long rlock_count;
    PyObject *in_weakreflist;
} rlockobject;

static int
rlock_traverse(rlockobject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    return 0;
}


static void
rlock_dealloc(rlockobject *self)
{
    PyObject_GC_UnTrack(self);
    if (self->in_weakreflist != NULL)
        PyObject_ClearWeakRefs((PyObject *) self);
    /* self->rlock_lock can be NULL if PyThread_allocate_lock() failed
       in rlock_new() */
    if (self->rlock_lock != NULL) {
        /* Unlock the lock so it's safe to free it */
        if (self->rlock_count > 0)
            PyThread_release_lock(self->rlock_lock);

        PyThread_free_lock(self->rlock_lock);
    }
    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free(self);
    Py_DECREF(tp);
}

static bool
rlock_is_owned_by(rlockobject *self, PyThread_ident_t tid)
{
    PyThread_ident_t owner_tid =
        _Py_atomic_load_ullong_relaxed(&self->rlock_owner);
    return owner_tid == tid && self->rlock_count > 0;
}

static PyObject *
rlock_acquire(rlockobject *self, PyObject *args, PyObject *kwds)
{
    PyTime_t timeout;
    PyThread_ident_t tid;
    PyLockStatus r = PY_LOCK_ACQUIRED;

    if (lock_acquire_parse_args(args, kwds, &timeout) < 0)
        return NULL;

    tid = PyThread_get_thread_ident_ex();
    if (rlock_is_owned_by(self, tid)) {
        unsigned long count = self->rlock_count + 1;
        if (count <= self->rlock_count) {
            PyErr_SetString(PyExc_OverflowError,
                            "Internal lock count overflowed");
            return NULL;
        }
        self->rlock_count = count;
        Py_RETURN_TRUE;
    }
    r = acquire_timed(self->rlock_lock, timeout);
    if (r == PY_LOCK_ACQUIRED) {
        assert(self->rlock_count == 0);
        _Py_atomic_store_ullong_relaxed(&self->rlock_owner, tid);
        self->rlock_count = 1;
    }
    else if (r == PY_LOCK_INTR) {
        return NULL;
    }

    return PyBool_FromLong(r == PY_LOCK_ACQUIRED);
}

PyDoc_STRVAR(rlock_acquire_doc,
"acquire(blocking=True) -> bool\n\
\n\
Lock the lock.  `blocking` indicates whether we should wait\n\
for the lock to be available or not.  If `blocking` is False\n\
and another thread holds the lock, the method will return False\n\
immediately.  If `blocking` is True and another thread holds\n\
the lock, the method will wait for the lock to be released,\n\
take it and then return True.\n\
(note: the blocking operation is interruptible.)\n\
\n\
In all other cases, the method will return True immediately.\n\
Precisely, if the current thread already holds the lock, its\n\
internal counter is simply incremented. If nobody holds the lock,\n\
the lock is taken and its internal counter initialized to 1.");

static PyObject *
rlock_release(rlockobject *self, PyObject *Py_UNUSED(ignored))
{
    PyThread_ident_t tid = PyThread_get_thread_ident_ex();

    if (!rlock_is_owned_by(self, tid)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "cannot release un-acquired lock");
        return NULL;
    }
    if (--self->rlock_count == 0) {
        _Py_atomic_store_ullong_relaxed(&self->rlock_owner, 0);
        PyThread_release_lock(self->rlock_lock);
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(rlock_release_doc,
"release()\n\
\n\
Release the lock, allowing another thread that is blocked waiting for\n\
the lock to acquire the lock.  The lock must be in the locked state,\n\
and must be locked by the same thread that unlocks it; otherwise a\n\
`RuntimeError` is raised.\n\
\n\
Do note that if the lock was acquire()d several times in a row by the\n\
current thread, release() needs to be called as many times for the lock\n\
to be available for other threads.");

static PyObject *
rlock_acquire_restore(rlockobject *self, PyObject *args)
{
    PyThread_ident_t owner;
    unsigned long count;
    int r = 1;

    if (!PyArg_ParseTuple(args, "(k" Py_PARSE_THREAD_IDENT_T "):_acquire_restore",
            &count, &owner))
        return NULL;

    if (!PyThread_acquire_lock(self->rlock_lock, 0)) {
        Py_BEGIN_ALLOW_THREADS
        r = PyThread_acquire_lock(self->rlock_lock, 1);
        Py_END_ALLOW_THREADS
    }
    if (!r) {
        PyErr_SetString(ThreadError, "couldn't acquire lock");
        return NULL;
    }
    assert(self->rlock_count == 0);
    _Py_atomic_store_ullong_relaxed(&self->rlock_owner, owner);
    self->rlock_count = count;
    Py_RETURN_NONE;
}

PyDoc_STRVAR(rlock_acquire_restore_doc,
"_acquire_restore(state) -> None\n\
\n\
For internal use by `threading.Condition`.");

static PyObject *
rlock_release_save(rlockobject *self, PyObject *Py_UNUSED(ignored))
{
    PyThread_ident_t owner;
    unsigned long count;

    if (self->rlock_count == 0) {
        PyErr_SetString(PyExc_RuntimeError,
                        "cannot release un-acquired lock");
        return NULL;
    }

    owner = self->rlock_owner;
    count = self->rlock_count;
    self->rlock_count = 0;
    _Py_atomic_store_ullong_relaxed(&self->rlock_owner, 0);
    PyThread_release_lock(self->rlock_lock);
    return Py_BuildValue("k" Py_PARSE_THREAD_IDENT_T, count, owner);
}

PyDoc_STRVAR(rlock_release_save_doc,
"_release_save() -> tuple\n\
\n\
For internal use by `threading.Condition`.");

static PyObject *
rlock_recursion_count(rlockobject *self, PyObject *Py_UNUSED(ignored))
{
    PyThread_ident_t tid = PyThread_get_thread_ident_ex();
    PyThread_ident_t owner =
        _Py_atomic_load_ullong_relaxed(&self->rlock_owner);
    return PyLong_FromUnsignedLong(owner == tid ? self->rlock_count : 0UL);
}

PyDoc_STRVAR(rlock_recursion_count_doc,
"_recursion_count() -> int\n\
\n\
For internal use by reentrancy checks.");

static PyObject *
rlock_is_owned(rlockobject *self, PyObject *Py_UNUSED(ignored))
{
    PyThread_ident_t tid = PyThread_get_thread_ident_ex();

    if (rlock_is_owned_by(self, tid)) {
        Py_RETURN_TRUE;
    }
    Py_RETURN_FALSE;
}

PyDoc_STRVAR(rlock_is_owned_doc,
"_is_owned() -> bool\n\
\n\
For internal use by `threading.Condition`.");

static PyObject *
rlock_new(PyTypeObject *type, PyObject *args, PyObject *kwds)
{
    rlockobject *self = (rlockobject *) type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }
    self->in_weakreflist = NULL;
    self->rlock_owner = 0;
    self->rlock_count = 0;

    self->rlock_lock = PyThread_allocate_lock();
    if (self->rlock_lock == NULL) {
        Py_DECREF(self);
        PyErr_SetString(ThreadError, "can't allocate lock");
        return NULL;
    }
    return (PyObject *) self;
}

static PyObject *
rlock_repr(rlockobject *self)
{
    PyThread_ident_t owner =
            _Py_atomic_load_ullong_relaxed(&self->rlock_owner);
    return PyUnicode_FromFormat(
        "<%s %s object owner=%" PY_FORMAT_THREAD_IDENT_T " count=%lu at %p>",
        self->rlock_count ? "locked" : "unlocked",
        Py_TYPE(self)->tp_name, owner,
        self->rlock_count, self);
}


#ifdef HAVE_FORK
static PyObject *
rlock__at_fork_reinit(rlockobject *self, PyObject *Py_UNUSED(args))
{
    if (_PyThread_at_fork_reinit(&self->rlock_lock) < 0) {
        PyErr_SetString(ThreadError, "failed to reinitialize lock at fork");
        return NULL;
    }

    self->rlock_owner = 0;
    self->rlock_count = 0;

    Py_RETURN_NONE;
}
#endif  /* HAVE_FORK */


static PyMethodDef rlock_methods[] = {
    {"acquire",      _PyCFunction_CAST(rlock_acquire),
     METH_VARARGS | METH_KEYWORDS, rlock_acquire_doc},
    {"release",      (PyCFunction)rlock_release,
     METH_NOARGS, rlock_release_doc},
    {"_is_owned",     (PyCFunction)rlock_is_owned,
     METH_NOARGS, rlock_is_owned_doc},
    {"_acquire_restore", (PyCFunction)rlock_acquire_restore,
     METH_VARARGS, rlock_acquire_restore_doc},
    {"_release_save", (PyCFunction)rlock_release_save,
     METH_NOARGS, rlock_release_save_doc},
    {"_recursion_count", (PyCFunction)rlock_recursion_count,
     METH_NOARGS, rlock_recursion_count_doc},
    {"__enter__",    _PyCFunction_CAST(rlock_acquire),
     METH_VARARGS | METH_KEYWORDS, rlock_acquire_doc},
    {"__exit__",    (PyCFunction)rlock_release,
     METH_VARARGS, rlock_release_doc},
#ifdef HAVE_FORK
    {"_at_fork_reinit",    (PyCFunction)rlock__at_fork_reinit,
     METH_NOARGS, NULL},
#endif
    {NULL,           NULL}              /* sentinel */
};


static PyMemberDef rlock_type_members[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(rlockobject, in_weakreflist), Py_READONLY},
    {NULL},
};

static PyType_Slot rlock_type_slots[] = {
    {Py_tp_dealloc, (destructor)rlock_dealloc},
    {Py_tp_repr, (reprfunc)rlock_repr},
    {Py_tp_methods, rlock_methods},
    {Py_tp_alloc, PyType_GenericAlloc},
    {Py_tp_new, rlock_new},
    {Py_tp_members, rlock_type_members},
    {Py_tp_traverse, rlock_traverse},
    {0, 0},
};

static PyType_Spec rlock_type_spec = {
    .name = "_thread.RLock",
    .basicsize = sizeof(rlockobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE |
              Py_TPFLAGS_HAVE_GC | Py_TPFLAGS_IMMUTABLETYPE),
    .slots = rlock_type_slots,
};

static lockobject *
newlockobject(PyObject *module)
{
    thread_module_state *state = get_thread_state(module);

    PyTypeObject *type = state->lock_type;
    lockobject *self = (lockobject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }

    self->lock_lock = PyThread_allocate_lock();
    self->locked = 0;
    self->in_weakreflist = NULL;

    if (self->lock_lock == NULL) {
        Py_DECREF(self);
        PyErr_SetString(ThreadError, "can't allocate lock");
        return NULL;
    }
    return self;
}

/* Thread-local objects */

/* Quick overview:

   We need to be able to reclaim reference cycles as soon as possible
   (both when a thread is being terminated, or a thread-local object
    becomes unreachable from user data).  Constraints:
   - it must not be possible for thread-state dicts to be involved in
     reference cycles (otherwise the cyclic GC will refuse to consider
     objects referenced from a reachable thread-state dict, even though
     local_dealloc would clear them)
   - the death of a thread-state dict must still imply destruction of the
     corresponding local dicts in all thread-local objects.

   Our implementation uses small "localdummy" objects in order to break
   the reference chain. These trivial objects are hashable (using the
   default scheme of identity hashing) and weakrefable.
   Each thread-state holds a separate localdummy for each local object
   (as a /strong reference/),
   and each thread-local object holds a dict mapping /weak references/
   of localdummies to local dicts.

   Therefore:
   - only the thread-state dict holds a strong reference to the dummies
   - only the thread-local object holds a strong reference to the local dicts
   - only outside objects (application- or library-level) hold strong
     references to the thread-local objects
   - as soon as a thread-state dict is destroyed, the weakref callbacks of all
     dummies attached to that thread are called, and destroy the corresponding
     local dicts from thread-local objects
   - as soon as a thread-local object is destroyed, its local dicts are
     destroyed and its dummies are manually removed from all thread states
   - the GC can do its work correctly when a thread-local object is dangling,
     without any interference from the thread-state dicts

   As an additional optimization, each localdummy holds a borrowed reference
   to the corresponding localdict.  This borrowed reference is only used
   by the thread-local object which has created the localdummy, which should
   guarantee that the localdict still exists when accessed.
*/

typedef struct {
    PyObject_HEAD
    PyObject *localdict;        /* Borrowed reference! */
    PyObject *weakreflist;      /* List of weak references to self */
} localdummyobject;

static void
localdummy_dealloc(localdummyobject *self)
{
    if (self->weakreflist != NULL)
        PyObject_ClearWeakRefs((PyObject *) self);
    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free((PyObject*)self);
    Py_DECREF(tp);
}

static PyMemberDef local_dummy_type_members[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(localdummyobject, weakreflist), Py_READONLY},
    {NULL},
};

static PyType_Slot local_dummy_type_slots[] = {
    {Py_tp_dealloc, (destructor)localdummy_dealloc},
    {Py_tp_doc, "Thread-local dummy"},
    {Py_tp_members, local_dummy_type_members},
    {0, 0}
};

static PyType_Spec local_dummy_type_spec = {
    .name = "_thread._localdummy",
    .basicsize = sizeof(localdummyobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_DISALLOW_INSTANTIATION |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = local_dummy_type_slots,
};


typedef struct {
    PyObject_HEAD
    PyObject *key;
    PyObject *args;
    PyObject *kw;
    PyObject *weakreflist;      /* List of weak references to self */
    /* A {localdummy weakref -> localdict} dict */
    PyObject *dummies;
    /* The callback for weakrefs to localdummies */
    PyObject *wr_callback;
} localobject;

/* Forward declaration */
static PyObject *_ldict(localobject *self, thread_module_state *state);
static PyObject *_localdummy_destroyed(PyObject *meth_self, PyObject *dummyweakref);

/* Create and register the dummy for the current thread.
   Returns a borrowed reference of the corresponding local dict */
static PyObject *
_local_create_dummy(localobject *self, thread_module_state *state)
{
    PyObject *ldict = NULL, *wr = NULL;
    localdummyobject *dummy = NULL;
    PyTypeObject *type = state->local_dummy_type;

    PyObject *tdict = PyThreadState_GetDict();
    if (tdict == NULL) {
        PyErr_SetString(PyExc_SystemError,
                        "Couldn't get thread-state dictionary");
        goto err;
    }

    ldict = PyDict_New();
    if (ldict == NULL) {
        goto err;
    }
    dummy = (localdummyobject *) type->tp_alloc(type, 0);
    if (dummy == NULL) {
        goto err;
    }
    dummy->localdict = ldict;
    wr = PyWeakref_NewRef((PyObject *) dummy, self->wr_callback);
    if (wr == NULL) {
        goto err;
    }

    /* As a side-effect, this will cache the weakref's hash before the
       dummy gets deleted */
    int r = PyDict_SetItem(self->dummies, wr, ldict);
    if (r < 0) {
        goto err;
    }
    Py_CLEAR(wr);
    r = PyDict_SetItem(tdict, self->key, (PyObject *) dummy);
    if (r < 0) {
        goto err;
    }
    Py_CLEAR(dummy);

    Py_DECREF(ldict);
    return ldict;

err:
    Py_XDECREF(ldict);
    Py_XDECREF(wr);
    Py_XDECREF(dummy);
    return NULL;
}

static PyObject *
local_new(PyTypeObject *type, PyObject *args, PyObject *kw)
{
    static PyMethodDef wr_callback_def = {
        "_localdummy_destroyed", (PyCFunction) _localdummy_destroyed, METH_O
    };

    if (type->tp_init == PyBaseObject_Type.tp_init) {
        int rc = 0;
        if (args != NULL)
            rc = PyObject_IsTrue(args);
        if (rc == 0 && kw != NULL)
            rc = PyObject_IsTrue(kw);
        if (rc != 0) {
            if (rc > 0) {
                PyErr_SetString(PyExc_TypeError,
                          "Initialization arguments are not supported");
            }
            return NULL;
        }
    }

    PyObject *module = PyType_GetModuleByDef(type, &thread_module);
    assert(module != NULL);
    thread_module_state *state = get_thread_state(module);

    localobject *self = (localobject *)type->tp_alloc(type, 0);
    if (self == NULL) {
        return NULL;
    }

    self->args = Py_XNewRef(args);
    self->kw = Py_XNewRef(kw);
    self->key = PyUnicode_FromFormat("thread.local.%p", self);
    if (self->key == NULL) {
        goto err;
    }

    self->dummies = PyDict_New();
    if (self->dummies == NULL) {
        goto err;
    }

    /* We use a weak reference to self in the callback closure
       in order to avoid spurious reference cycles */
    PyObject *wr = PyWeakref_NewRef((PyObject *) self, NULL);
    if (wr == NULL) {
        goto err;
    }
    self->wr_callback = PyCFunction_NewEx(&wr_callback_def, wr, NULL);
    Py_DECREF(wr);
    if (self->wr_callback == NULL) {
        goto err;
    }
    if (_local_create_dummy(self, state) == NULL) {
        goto err;
    }
    return (PyObject *)self;

  err:
    Py_DECREF(self);
    return NULL;
}

static int
local_traverse(localobject *self, visitproc visit, void *arg)
{
    Py_VISIT(Py_TYPE(self));
    Py_VISIT(self->args);
    Py_VISIT(self->kw);
    Py_VISIT(self->dummies);
    return 0;
}

static int
local_clear(localobject *self)
{
    Py_CLEAR(self->args);
    Py_CLEAR(self->kw);
    Py_CLEAR(self->dummies);
    Py_CLEAR(self->wr_callback);
    /* Remove all strong references to dummies from the thread states */
    if (self->key) {
        PyInterpreterState *interp = _PyInterpreterState_GET();
        _PyRuntimeState *runtime = &_PyRuntime;
        HEAD_LOCK(runtime);
        PyThreadState *tstate = PyInterpreterState_ThreadHead(interp);
        HEAD_UNLOCK(runtime);
        while (tstate) {
            if (tstate->dict) {
                if (PyDict_Pop(tstate->dict, self->key, NULL) < 0) {
                    // Silently ignore error
                    PyErr_Clear();
                }
            }
            HEAD_LOCK(runtime);
            tstate = PyThreadState_Next(tstate);
            HEAD_UNLOCK(runtime);
        }
    }
    return 0;
}

static void
local_dealloc(localobject *self)
{
    /* Weakrefs must be invalidated right now, otherwise they can be used
       from code called below, which is very dangerous since Py_REFCNT(self) == 0 */
    if (self->weakreflist != NULL) {
        PyObject_ClearWeakRefs((PyObject *) self);
    }

    PyObject_GC_UnTrack(self);

    local_clear(self);
    Py_XDECREF(self->key);

    PyTypeObject *tp = Py_TYPE(self);
    tp->tp_free((PyObject*)self);
    Py_DECREF(tp);
}

/* Returns a borrowed reference to the local dict, creating it if necessary */
static PyObject *
_ldict(localobject *self, thread_module_state *state)
{
    PyObject *tdict = PyThreadState_GetDict();
    if (tdict == NULL) {
        PyErr_SetString(PyExc_SystemError,
                        "Couldn't get thread-state dictionary");
        return NULL;
    }

    PyObject *ldict;
    PyObject *dummy = PyDict_GetItemWithError(tdict, self->key);
    if (dummy == NULL) {
        if (PyErr_Occurred()) {
            return NULL;
        }
        ldict = _local_create_dummy(self, state);
        if (ldict == NULL)
            return NULL;

        if (Py_TYPE(self)->tp_init != PyBaseObject_Type.tp_init &&
            Py_TYPE(self)->tp_init((PyObject*)self,
                                   self->args, self->kw) < 0) {
            /* we need to get rid of ldict from thread so
               we create a new one the next time we do an attr
               access */
            PyDict_DelItem(tdict, self->key);
            return NULL;
        }
    }
    else {
        assert(Py_IS_TYPE(dummy, state->local_dummy_type));
        ldict = ((localdummyobject *) dummy)->localdict;
    }

    return ldict;
}

static int
local_setattro(localobject *self, PyObject *name, PyObject *v)
{
    PyObject *module = PyType_GetModuleByDef(Py_TYPE(self), &thread_module);
    assert(module != NULL);
    thread_module_state *state = get_thread_state(module);

    PyObject *ldict = _ldict(self, state);
    if (ldict == NULL) {
        return -1;
    }

    int r = PyObject_RichCompareBool(name, &_Py_ID(__dict__), Py_EQ);
    if (r == -1) {
        return -1;
    }
    if (r == 1) {
        PyErr_Format(PyExc_AttributeError,
                     "'%.100s' object attribute '%U' is read-only",
                     Py_TYPE(self)->tp_name, name);
        return -1;
    }

    return _PyObject_GenericSetAttrWithDict((PyObject *)self, name, v, ldict);
}

static PyObject *local_getattro(localobject *, PyObject *);

static PyMemberDef local_type_members[] = {
    {"__weaklistoffset__", Py_T_PYSSIZET, offsetof(localobject, weakreflist), Py_READONLY},
    {NULL},
};

static PyType_Slot local_type_slots[] = {
    {Py_tp_dealloc, (destructor)local_dealloc},
    {Py_tp_getattro, (getattrofunc)local_getattro},
    {Py_tp_setattro, (setattrofunc)local_setattro},
    {Py_tp_doc, "Thread-local data"},
    {Py_tp_traverse, (traverseproc)local_traverse},
    {Py_tp_clear, (inquiry)local_clear},
    {Py_tp_new, local_new},
    {Py_tp_members, local_type_members},
    {0, 0}
};

static PyType_Spec local_type_spec = {
    .name = "_thread._local",
    .basicsize = sizeof(localobject),
    .flags = (Py_TPFLAGS_DEFAULT | Py_TPFLAGS_BASETYPE | Py_TPFLAGS_HAVE_GC |
              Py_TPFLAGS_IMMUTABLETYPE),
    .slots = local_type_slots,
};

static PyObject *
local_getattro(localobject *self, PyObject *name)
{
    PyObject *module = PyType_GetModuleByDef(Py_TYPE(self), &thread_module);
    assert(module != NULL);
    thread_module_state *state = get_thread_state(module);

    PyObject *ldict = _ldict(self, state);
    if (ldict == NULL)
        return NULL;

    int r = PyObject_RichCompareBool(name, &_Py_ID(__dict__), Py_EQ);
    if (r == 1) {
        return Py_NewRef(ldict);
    }
    if (r == -1) {
        return NULL;
    }

    if (!Py_IS_TYPE(self, state->local_type)) {
        /* use generic lookup for subtypes */
        return _PyObject_GenericGetAttrWithDict((PyObject *)self, name,
                                                ldict, 0);
    }

    /* Optimization: just look in dict ourselves */
    PyObject *value;
    if (PyDict_GetItemRef(ldict, name, &value) != 0) {
        // found or error
        return value;
    }

    /* Fall back on generic to get __class__ and __dict__ */
    return _PyObject_GenericGetAttrWithDict(
        (PyObject *)self, name, ldict, 0);
}

/* Called when a dummy is destroyed. */
static PyObject *
_localdummy_destroyed(PyObject *localweakref, PyObject *dummyweakref)
{
    localobject *self = (localobject *)_PyWeakref_GET_REF(localweakref);
    if (self == NULL) {
        Py_RETURN_NONE;
    }

    /* If the thread-local object is still alive and not being cleared,
       remove the corresponding local dict */
    if (self->dummies != NULL) {
        PyObject *ldict;
        ldict = PyDict_GetItemWithError(self->dummies, dummyweakref);
        if (ldict != NULL) {
            PyDict_DelItem(self->dummies, dummyweakref);
        }
        if (PyErr_Occurred())
            PyErr_WriteUnraisable((PyObject*)self);
    }
    Py_DECREF(self);
    Py_RETURN_NONE;
}

/* Module functions */

struct bootstate {
    PyThreadState *tstate;
    PyObject *func;
    PyObject *args;
    PyObject *kwargs;
};


static void
thread_bootstate_free(struct bootstate *boot, int decref)
{
    if (decref) {
        Py_DECREF(boot->func);
        Py_DECREF(boot->args);
        Py_XDECREF(boot->kwargs);
    }
    PyMem_RawFree(boot);
}


static void
thread_run(void *boot_raw)
{
    struct bootstate *boot = (struct bootstate *) boot_raw;
    PyThreadState *tstate = boot->tstate;

    // gh-108987: If _thread.start_new_thread() is called before or while
    // Python is being finalized, thread_run() can called *after*.
    // _PyRuntimeState_SetFinalizing() is called. At this point, all Python
    // threads must exit, except of the thread calling Py_Finalize() whch holds
    // the GIL and must not exit.
    //
    // At this stage, tstate can be a dangling pointer (point to freed memory),
    // it's ok to call _PyThreadState_MustExit() with a dangling pointer.
    if (_PyThreadState_MustExit(tstate)) {
        // Don't call PyThreadState_Clear() nor _PyThreadState_DeleteCurrent().
        // These functions are called on tstate indirectly by Py_Finalize()
        // which calls _PyInterpreterState_Clear().
        //
        // Py_DECREF() cannot be called because the GIL is not held: leak
        // references on purpose. Python is being finalized anyway.
        thread_bootstate_free(boot, 0);
        goto exit;
    }

    _PyThreadState_Bind(tstate);
    PyEval_AcquireThread(tstate);
    _Py_atomic_add_ssize(&tstate->interp->threads.count, 1);

    PyObject *res = PyObject_Call(boot->func, boot->args, boot->kwargs);
    if (res == NULL) {
        if (PyErr_ExceptionMatches(PyExc_SystemExit))
            /* SystemExit is ignored silently */
            PyErr_Clear();
        else {
            PyErr_FormatUnraisable(
                "Exception ignored in thread started by %R", boot->func);
        }
    }
    else {
        Py_DECREF(res);
    }

    thread_bootstate_free(boot, 1);

    _Py_atomic_add_ssize(&tstate->interp->threads.count, -1);
    PyThreadState_Clear(tstate);
    _PyThreadState_DeleteCurrent(tstate);

exit:
    // bpo-44434: Don't call explicitly PyThread_exit_thread(). On Linux with
    // the glibc, pthread_exit() can abort the whole process if dlopen() fails
    // to open the libgcc_s.so library (ex: EMFILE error).
    return;
}

static PyObject *
thread_daemon_threads_allowed(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (interp->feature_flags & Py_RTFLAGS_DAEMON_THREADS) {
        Py_RETURN_TRUE;
    }
    else {
        Py_RETURN_FALSE;
    }
}

PyDoc_STRVAR(daemon_threads_allowed_doc,
"daemon_threads_allowed()\n\
\n\
Return True if daemon threads are allowed in the current interpreter,\n\
and False otherwise.\n");

static int
do_start_new_thread(thread_module_state* state,
                    PyObject *func, PyObject* args, PyObject* kwargs,
                    int joinable,
                    PyThread_ident_t* ident, PyThread_handle_t* handle)
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    if (!_PyInterpreterState_HasFeature(interp, Py_RTFLAGS_THREADS)) {
        PyErr_SetString(PyExc_RuntimeError,
                        "thread is not supported for isolated subinterpreters");
        return -1;
    }
    if (interp->finalizing) {
        PyErr_SetString(PyExc_PythonFinalizationError,
                        "can't create new thread at interpreter shutdown");
        return -1;
    }

    // gh-109795: Use PyMem_RawMalloc() instead of PyMem_Malloc(),
    // because it should be possible to call thread_bootstate_free()
    // without holding the GIL.
    struct bootstate *boot = PyMem_RawMalloc(sizeof(struct bootstate));
    if (boot == NULL) {
        PyErr_NoMemory();
        return -1;
    }
    boot->tstate = _PyThreadState_New(interp, _PyThreadState_WHENCE_THREADING);
    if (boot->tstate == NULL) {
        PyMem_RawFree(boot);
        if (!PyErr_Occurred()) {
            PyErr_NoMemory();
        }
        return -1;
    }
    boot->func = Py_NewRef(func);
    boot->args = Py_NewRef(args);
    boot->kwargs = Py_XNewRef(kwargs);

    int err;
    if (joinable) {
        err = PyThread_start_joinable_thread(thread_run, (void*) boot, ident, handle);
    } else {
        *handle = 0;
        *ident = PyThread_start_new_thread(thread_run, (void*) boot);
        err = (*ident == PYTHREAD_INVALID_THREAD_ID);
    }
    if (err) {
        PyErr_SetString(ThreadError, "can't start new thread");
        PyThreadState_Clear(boot->tstate);
        thread_bootstate_free(boot, 1);
        return -1;
    }
    return 0;
}

static PyObject *
thread_PyThread_start_new_thread(PyObject *module, PyObject *fargs)
{
    PyObject *func, *args, *kwargs = NULL;
    thread_module_state *state = get_thread_state(module);

    if (!PyArg_UnpackTuple(fargs, "start_new_thread", 2, 3,
                           &func, &args, &kwargs))
        return NULL;
    if (!PyCallable_Check(func)) {
        PyErr_SetString(PyExc_TypeError,
                        "first arg must be callable");
        return NULL;
    }
    if (!PyTuple_Check(args)) {
        PyErr_SetString(PyExc_TypeError,
                        "2nd arg must be a tuple");
        return NULL;
    }
    if (kwargs != NULL && !PyDict_Check(kwargs)) {
        PyErr_SetString(PyExc_TypeError,
                        "optional 3rd arg must be a dictionary");
        return NULL;
    }

    if (PySys_Audit("_thread.start_new_thread", "OOO",
                    func, args, kwargs ? kwargs : Py_None) < 0) {
        return NULL;
    }

    PyThread_ident_t ident = 0;
    PyThread_handle_t handle;
    if (do_start_new_thread(state, func, args, kwargs, /*joinable=*/ 0,
                            &ident, &handle)) {
        return NULL;
    }
    return PyLong_FromUnsignedLongLong(ident);
}

PyDoc_STRVAR(start_new_doc,
"start_new_thread(function, args[, kwargs])\n\
(start_new() is an obsolete synonym)\n\
\n\
Start a new thread and return its identifier.\n\
\n\
The thread will call the function with positional arguments from the\n\
tuple args and keyword arguments taken from the optional dictionary\n\
kwargs.  The thread exits when the function returns; the return value\n\
is ignored.  The thread will also exit when the function raises an\n\
unhandled exception; a stack trace will be printed unless the exception\n\
is SystemExit.\n");

static PyObject *
thread_PyThread_start_joinable_thread(PyObject *module, PyObject *func)
{
    thread_module_state *state = get_thread_state(module);

    if (!PyCallable_Check(func)) {
        PyErr_SetString(PyExc_TypeError,
                        "thread function must be callable");
        return NULL;
    }

    if (PySys_Audit("_thread.start_joinable_thread", "O", func) < 0) {
        return NULL;
    }

    PyObject* args = PyTuple_New(0);
    if (args == NULL) {
        return NULL;
    }
    ThreadHandleObject* hobj = new_thread_handle(state);
    if (hobj == NULL) {
        Py_DECREF(args);
        return NULL;
    }
    if (do_start_new_thread(state, func, args, /*kwargs=*/ NULL, /*joinable=*/ 1,
                            &hobj->ident, &hobj->handle)) {
        Py_DECREF(args);
        Py_DECREF(hobj);
        return NULL;
    }
    Py_DECREF(args);
    hobj->joinable = 1;
    return (PyObject*) hobj;
}

PyDoc_STRVAR(start_joinable_doc,
"start_joinable_thread(function)\n\
\n\
*For internal use only*: start a new thread.\n\
\n\
Like start_new_thread(), this starts a new thread calling the given function.\n\
Unlike start_new_thread(), this returns a handle object with methods to join\n\
or detach the given thread.\n\
This function is not for third-party code, please use the\n\
`threading` module instead.\n");

static PyObject *
thread_PyThread_exit_thread(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyErr_SetNone(PyExc_SystemExit);
    return NULL;
}

PyDoc_STRVAR(exit_doc,
"exit()\n\
(exit_thread() is an obsolete synonym)\n\
\n\
This is synonymous to ``raise SystemExit''.  It will cause the current\n\
thread to exit silently unless the exception is caught.");

static PyObject *
thread_PyThread_interrupt_main(PyObject *self, PyObject *args)
{
    int signum = SIGINT;
    if (!PyArg_ParseTuple(args, "|i:signum", &signum)) {
        return NULL;
    }

    if (PyErr_SetInterruptEx(signum)) {
        PyErr_SetString(PyExc_ValueError, "signal number out of range");
        return NULL;
    }
    Py_RETURN_NONE;
}

PyDoc_STRVAR(interrupt_doc,
"interrupt_main(signum=signal.SIGINT, /)\n\
\n\
Simulate the arrival of the given signal in the main thread,\n\
where the corresponding signal handler will be executed.\n\
If *signum* is omitted, SIGINT is assumed.\n\
A subthread can use this function to interrupt the main thread.\n\
\n\
Note: the default signal handler for SIGINT raises ``KeyboardInterrupt``."
);

static PyObject *
thread_PyThread_allocate_lock(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    return (PyObject *) newlockobject(module);
}

PyDoc_STRVAR(allocate_doc,
"allocate_lock() -> lock object\n\
(allocate() is an obsolete synonym)\n\
\n\
Create a new lock object. See help(type(threading.Lock())) for\n\
information about locks.");

static PyObject *
thread_get_ident(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyThread_ident_t ident = PyThread_get_thread_ident_ex();
    if (ident == PYTHREAD_INVALID_THREAD_ID) {
        PyErr_SetString(ThreadError, "no current thread ident");
        return NULL;
    }
    return PyLong_FromUnsignedLongLong(ident);
}

PyDoc_STRVAR(get_ident_doc,
"get_ident() -> integer\n\
\n\
Return a non-zero integer that uniquely identifies the current thread\n\
amongst other threads that exist simultaneously.\n\
This may be used to identify per-thread resources.\n\
Even though on some platforms threads identities may appear to be\n\
allocated consecutive numbers starting at 1, this behavior should not\n\
be relied upon, and the number should be seen purely as a magic cookie.\n\
A thread's identity may be reused for another thread after it exits.");

#ifdef PY_HAVE_THREAD_NATIVE_ID
static PyObject *
thread_get_native_id(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    unsigned long native_id = PyThread_get_thread_native_id();
    return PyLong_FromUnsignedLong(native_id);
}

PyDoc_STRVAR(get_native_id_doc,
"get_native_id() -> integer\n\
\n\
Return a non-negative integer identifying the thread as reported\n\
by the OS (kernel). This may be used to uniquely identify a\n\
particular thread within a system.");
#endif

static PyObject *
thread__count(PyObject *self, PyObject *Py_UNUSED(ignored))
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return PyLong_FromSsize_t(_Py_atomic_load_ssize(&interp->threads.count));
}

PyDoc_STRVAR(_count_doc,
"_count() -> integer\n\
\n\
\
Return the number of currently running Python threads, excluding\n\
the main thread. The returned number comprises all threads created\n\
through `start_new_thread()` as well as `threading.Thread`, and not\n\
yet finished.\n\
\n\
This function is meant for internal and specialized purposes only.\n\
In most applications `threading.enumerate()` should be used instead.");

static void
release_sentinel(void *weakref_raw)
{
    PyObject *weakref = _PyObject_CAST(weakref_raw);

    /* Tricky: this function is called when the current thread state
       is being deleted.  Therefore, only simple C code can safely
       execute here. */
    lockobject *lock = (lockobject *)_PyWeakref_GET_REF(weakref);
    if (lock != NULL) {
        if (lock->locked) {
            PyThread_release_lock(lock->lock_lock);
            lock->locked = 0;
        }
        Py_DECREF(lock);
    }

    /* Deallocating a weakref with a NULL callback only calls
       PyObject_GC_Del(), which can't call any Python code. */
    Py_DECREF(weakref);
}

static PyObject *
thread__set_sentinel(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyObject *wr;
    PyThreadState *tstate = _PyThreadState_GET();
    lockobject *lock;

    if (tstate->on_delete_data != NULL) {
        /* We must support the re-creation of the lock from a
           fork()ed child. */
        assert(tstate->on_delete == &release_sentinel);
        wr = (PyObject *) tstate->on_delete_data;
        tstate->on_delete = NULL;
        tstate->on_delete_data = NULL;
        Py_DECREF(wr);
    }
    lock = newlockobject(module);
    if (lock == NULL)
        return NULL;
    /* The lock is owned by whoever called _set_sentinel(), but the weakref
       hangs to the thread state. */
    wr = PyWeakref_NewRef((PyObject *) lock, NULL);
    if (wr == NULL) {
        Py_DECREF(lock);
        return NULL;
    }
    tstate->on_delete_data = (void *) wr;
    tstate->on_delete = &release_sentinel;
    return (PyObject *) lock;
}

PyDoc_STRVAR(_set_sentinel_doc,
"_set_sentinel() -> lock\n\
\n\
Set a sentinel lock that will be released when the current thread\n\
state is finalized (after it is untied from the interpreter).\n\
\n\
This is a private API for the threading module.");

static PyObject *
thread_stack_size(PyObject *self, PyObject *args)
{
    size_t old_size;
    Py_ssize_t new_size = 0;
    int rc;

    if (!PyArg_ParseTuple(args, "|n:stack_size", &new_size))
        return NULL;

    if (new_size < 0) {
        PyErr_SetString(PyExc_ValueError,
                        "size must be 0 or a positive value");
        return NULL;
    }

    old_size = PyThread_get_stacksize();

    rc = PyThread_set_stacksize((size_t) new_size);
    if (rc == -1) {
        PyErr_Format(PyExc_ValueError,
                     "size not valid: %zd bytes",
                     new_size);
        return NULL;
    }
    if (rc == -2) {
        PyErr_SetString(ThreadError,
                        "setting stack size not supported");
        return NULL;
    }

    return PyLong_FromSsize_t((Py_ssize_t) old_size);
}

PyDoc_STRVAR(stack_size_doc,
"stack_size([size]) -> size\n\
\n\
Return the thread stack size used when creating new threads.  The\n\
optional size argument specifies the stack size (in bytes) to be used\n\
for subsequently created threads, and must be 0 (use platform or\n\
configured default) or a positive integer value of at least 32,768 (32k).\n\
If changing the thread stack size is unsupported, a ThreadError\n\
exception is raised.  If the specified size is invalid, a ValueError\n\
exception is raised, and the stack size is unmodified.  32k bytes\n\
 currently the minimum supported stack size value to guarantee\n\
sufficient stack space for the interpreter itself.\n\
\n\
Note that some platforms may have particular restrictions on values for\n\
the stack size, such as requiring a minimum stack size larger than 32 KiB or\n\
requiring allocation in multiples of the system memory page size\n\
- platform documentation should be referred to for more information\n\
(4 KiB pages are common; using multiples of 4096 for the stack size is\n\
the suggested approach in the absence of more specific information).");

static int
thread_excepthook_file(PyObject *file, PyObject *exc_type, PyObject *exc_value,
                       PyObject *exc_traceback, PyObject *thread)
{
    /* print(f"Exception in thread {thread.name}:", file=file) */
    if (PyFile_WriteString("Exception in thread ", file) < 0) {
        return -1;
    }

    PyObject *name = NULL;
    if (thread != Py_None) {
        if (PyObject_GetOptionalAttr(thread, &_Py_ID(name), &name) < 0) {
            return -1;
        }
    }
    if (name != NULL) {
        if (PyFile_WriteObject(name, file, Py_PRINT_RAW) < 0) {
            Py_DECREF(name);
            return -1;
        }
        Py_DECREF(name);
    }
    else {
        PyThread_ident_t ident = PyThread_get_thread_ident_ex();
        PyObject *str = PyUnicode_FromFormat("%" PY_FORMAT_THREAD_IDENT_T, ident);
        if (str != NULL) {
            if (PyFile_WriteObject(str, file, Py_PRINT_RAW) < 0) {
                Py_DECREF(str);
                return -1;
            }
            Py_DECREF(str);
        }
        else {
            PyErr_Clear();

            if (PyFile_WriteString("<failed to get thread name>", file) < 0) {
                return -1;
            }
        }
    }

    if (PyFile_WriteString(":\n", file) < 0) {
        return -1;
    }

    /* Display the traceback */
    _PyErr_Display(file, exc_type, exc_value, exc_traceback);

    /* Call file.flush() */
    if (_PyFile_Flush(file) < 0) {
        return -1;
    }

    return 0;
}


PyDoc_STRVAR(ExceptHookArgs__doc__,
"ExceptHookArgs\n\
\n\
Type used to pass arguments to threading.excepthook.");

static PyStructSequence_Field ExceptHookArgs_fields[] = {
    {"exc_type", "Exception type"},
    {"exc_value", "Exception value"},
    {"exc_traceback", "Exception traceback"},
    {"thread", "Thread"},
    {0}
};

static PyStructSequence_Desc ExceptHookArgs_desc = {
    .name = "_thread._ExceptHookArgs",
    .doc = ExceptHookArgs__doc__,
    .fields = ExceptHookArgs_fields,
    .n_in_sequence = 4
};


static PyObject *
thread_excepthook(PyObject *module, PyObject *args)
{
    thread_module_state *state = get_thread_state(module);

    if (!Py_IS_TYPE(args, state->excepthook_type)) {
        PyErr_SetString(PyExc_TypeError,
                        "_thread.excepthook argument type "
                        "must be ExceptHookArgs");
        return NULL;
    }

    /* Borrowed reference */
    PyObject *exc_type = PyStructSequence_GET_ITEM(args, 0);
    if (exc_type == PyExc_SystemExit) {
        /* silently ignore SystemExit */
        Py_RETURN_NONE;
    }

    /* Borrowed references */
    PyObject *exc_value = PyStructSequence_GET_ITEM(args, 1);
    PyObject *exc_tb = PyStructSequence_GET_ITEM(args, 2);
    PyObject *thread = PyStructSequence_GET_ITEM(args, 3);

    PyThreadState *tstate = _PyThreadState_GET();
    PyObject *file = _PySys_GetAttr(tstate, &_Py_ID(stderr));
    if (file == NULL || file == Py_None) {
        if (thread == Py_None) {
            /* do nothing if sys.stderr is None and thread is None */
            Py_RETURN_NONE;
        }

        file = PyObject_GetAttrString(thread, "_stderr");
        if (file == NULL) {
            return NULL;
        }
        if (file == Py_None) {
            Py_DECREF(file);
            /* do nothing if sys.stderr is None and sys.stderr was None
               when the thread was created */
            Py_RETURN_NONE;
        }
    }
    else {
        Py_INCREF(file);
    }

    int res = thread_excepthook_file(file, exc_type, exc_value, exc_tb,
                                     thread);
    Py_DECREF(file);
    if (res < 0) {
        return NULL;
    }

    Py_RETURN_NONE;
}

PyDoc_STRVAR(excepthook_doc,
"excepthook(exc_type, exc_value, exc_traceback, thread)\n\
\n\
Handle uncaught Thread.run() exception.");

static PyObject *
thread__is_main_interpreter(PyObject *module, PyObject *Py_UNUSED(ignored))
{
    PyInterpreterState *interp = _PyInterpreterState_GET();
    return PyBool_FromLong(_Py_IsMainInterpreter(interp));
}

PyDoc_STRVAR(thread__is_main_interpreter_doc,
"_is_main_interpreter()\n\
\n\
Return True if the current interpreter is the main Python interpreter.");

static PyMethodDef thread_methods[] = {
    {"start_new_thread",        (PyCFunction)thread_PyThread_start_new_thread,
     METH_VARARGS, start_new_doc},
    {"start_new",               (PyCFunction)thread_PyThread_start_new_thread,
     METH_VARARGS, start_new_doc},
    {"start_joinable_thread",   (PyCFunction)thread_PyThread_start_joinable_thread,
     METH_O, start_joinable_doc},
    {"daemon_threads_allowed",  (PyCFunction)thread_daemon_threads_allowed,
     METH_NOARGS, daemon_threads_allowed_doc},
    {"allocate_lock",           thread_PyThread_allocate_lock,
     METH_NOARGS, allocate_doc},
    {"allocate",                thread_PyThread_allocate_lock,
     METH_NOARGS, allocate_doc},
    {"exit_thread",             thread_PyThread_exit_thread,
     METH_NOARGS, exit_doc},
    {"exit",                    thread_PyThread_exit_thread,
     METH_NOARGS, exit_doc},
    {"interrupt_main",          (PyCFunction)thread_PyThread_interrupt_main,
     METH_VARARGS, interrupt_doc},
    {"get_ident",               thread_get_ident,
     METH_NOARGS, get_ident_doc},
#ifdef PY_HAVE_THREAD_NATIVE_ID
    {"get_native_id",           thread_get_native_id,
     METH_NOARGS, get_native_id_doc},
#endif
    {"_count",                  thread__count,
     METH_NOARGS, _count_doc},
    {"stack_size",              (PyCFunction)thread_stack_size,
     METH_VARARGS, stack_size_doc},
    {"_set_sentinel",           thread__set_sentinel,
     METH_NOARGS, _set_sentinel_doc},
    {"_excepthook",             thread_excepthook,
     METH_O, excepthook_doc},
    {"_is_main_interpreter",    thread__is_main_interpreter,
     METH_NOARGS, thread__is_main_interpreter_doc},
    {NULL,                      NULL}           /* sentinel */
};


/* Initialization function */

static int
thread_module_exec(PyObject *module)
{
    thread_module_state *state = get_thread_state(module);
    PyObject *d = PyModule_GetDict(module);

    // Initialize the C thread library
    PyThread_init_thread();

    // _ThreadHandle
    state->thread_handle_type = (PyTypeObject *)PyType_FromSpec(&ThreadHandle_Type_spec);
    if (state->thread_handle_type == NULL) {
        return -1;
    }
    if (PyDict_SetItemString(d, "_ThreadHandle", (PyObject *)state->thread_handle_type) < 0) {
        return -1;
    }

    // Lock
    state->lock_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &lock_type_spec, NULL);
    if (state->lock_type == NULL) {
        return -1;
    }
    if (PyModule_AddType(module, state->lock_type) < 0) {
        return -1;
    }
    // Old alias: lock -> LockType
    if (PyDict_SetItemString(d, "LockType", (PyObject *)state->lock_type) < 0) {
        return -1;
    }

    // RLock
    PyTypeObject *rlock_type = (PyTypeObject *)PyType_FromSpec(&rlock_type_spec);
    if (rlock_type == NULL) {
        return -1;
    }
    if (PyModule_AddType(module, rlock_type) < 0) {
        Py_DECREF(rlock_type);
        return -1;
    }
    Py_DECREF(rlock_type);

    // Local dummy
    state->local_dummy_type = (PyTypeObject *)PyType_FromSpec(&local_dummy_type_spec);
    if (state->local_dummy_type == NULL) {
        return -1;
    }

    // Local
    state->local_type = (PyTypeObject *)PyType_FromModuleAndSpec(module, &local_type_spec, NULL);
    if (state->local_type == NULL) {
        return -1;
    }
    if (PyModule_AddType(module, state->local_type) < 0) {
        return -1;
    }

    // Add module attributes
    if (PyDict_SetItemString(d, "error", ThreadError) < 0) {
        return -1;
    }

    // _ExceptHookArgs type
    state->excepthook_type = PyStructSequence_NewType(&ExceptHookArgs_desc);
    if (state->excepthook_type == NULL) {
        return -1;
    }
    if (PyModule_AddType(module, state->excepthook_type) < 0) {
        return -1;
    }

    // TIMEOUT_MAX
    double timeout_max = (double)PY_TIMEOUT_MAX * 1e-6;
    double time_max = PyTime_AsSecondsDouble(PyTime_MAX);
    timeout_max = Py_MIN(timeout_max, time_max);
    // Round towards minus infinity
    timeout_max = floor(timeout_max);

    if (PyModule_Add(module, "TIMEOUT_MAX",
                        PyFloat_FromDouble(timeout_max)) < 0) {
        return -1;
    }

    return 0;
}


static int
thread_module_traverse(PyObject *module, visitproc visit, void *arg)
{
    thread_module_state *state = get_thread_state(module);
    Py_VISIT(state->excepthook_type);
    Py_VISIT(state->lock_type);
    Py_VISIT(state->local_type);
    Py_VISIT(state->local_dummy_type);
    Py_VISIT(state->thread_handle_type);
    return 0;
}

static int
thread_module_clear(PyObject *module)
{
    thread_module_state *state = get_thread_state(module);
    Py_CLEAR(state->excepthook_type);
    Py_CLEAR(state->lock_type);
    Py_CLEAR(state->local_type);
    Py_CLEAR(state->local_dummy_type);
    Py_CLEAR(state->thread_handle_type);
    return 0;
}

static void
thread_module_free(void *module)
{
    thread_module_clear((PyObject *)module);
}



PyDoc_STRVAR(thread_doc,
"This module provides primitive operations to write multi-threaded programs.\n\
The 'threading' module provides a more convenient interface.");

static PyModuleDef_Slot thread_module_slots[] = {
    {Py_mod_exec, thread_module_exec},
    {Py_mod_multiple_interpreters, Py_MOD_PER_INTERPRETER_GIL_SUPPORTED},
    {0, NULL}
};

static struct PyModuleDef thread_module = {
    PyModuleDef_HEAD_INIT,
    .m_name = "_thread",
    .m_doc = thread_doc,
    .m_size = sizeof(thread_module_state),
    .m_methods = thread_methods,
    .m_traverse = thread_module_traverse,
    .m_clear = thread_module_clear,
    .m_free = thread_module_free,
    .m_slots = thread_module_slots,
};

PyMODINIT_FUNC
PyInit__thread(void)
{
    return PyModuleDef_Init(&thread_module);
}
