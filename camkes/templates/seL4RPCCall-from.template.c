/*#
 *# Copyright 2014, NICTA
 *#
 *# This software may be distributed and modified according to the terms of
 *# the BSD 2-Clause license. Note that NO WARRANTY is provided.
 *# See "LICENSE_BSD2.txt" for details.
 *#
 *# @TAG(NICTA_BSD)
 #*/

#define _POSIX_SOURCE /* stpcpy */
#include <sel4/sel4.h>
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sync/sem-bare.h>
#include <camkes/marshal.h>
#include <camkes/dataport.h>
#include <camkes/error.h>

/*? macros.show_includes(me.from_instance.type.includes) ?*/
/*? macros.show_includes(me.from_interface.type.includes, '../static/components/' + me.from_instance.type.name + '/') ?*/

/*- set ep = alloc('ep', seL4_EndpointObject, write=True, grant=True) -*/
/*- for s in configuration.settings -*/
    /*- if s.instance == me.from_instance.name -*/
        /*- if s.attribute == "%s_attributes" % (me.from_interface.name) -*/
            /*- set badge = s.value.strip('"') -*/
            /*- do cap_space.cnode[ep].set_badge(int(badge, 0)) -*/
        /*- endif -*/
    /*- endif -*/
/*- endfor -*/

/*# Determine if we trust our partner. If we trust them, we can be more liberal
 *# with error checking.
 #*/
/*- set _trust_partner = [False] -*/
/*- for s in configuration.settings -*/
    /*- if s.instance == me.to_instance.name and s.attribute == 'trusted' and s.value == '"true"' -*/
        /*- do _trust_partner.__setitem__(0, True) -*/
    /*- endif -*/
/*- endfor -*/
/*- set trust_partner = _trust_partner[0] -*/

/*- set BUFFER_BASE = c_symbol('BUFFER_BASE') -*/
/*- set base = '((void*)&seL4_GetIPCBuffer()->msg[0])' -*/
/*- set userspace_ipc = False -*/
/*- if configuration -*/
    /*- set buffers = filter(lambda('x: x.instance == \'%s\' and x.attribute == \'%s_buffer\'' % (me.from_instance.name, me.from_interface.name)), configuration.settings) -*/
    /*- if len(buffers) == 1 -*/
        /*- set base = buffers[0].value -*/
        /*- set userspace_ipc = True -*/
        extern void * /*? base ?*/;
    /*- endif -*/
/*- endif -*/
#define /*? BUFFER_BASE ?*/ /*? base ?*/

/*- set methods_len = len(me.from_interface.type.methods) -*/
/*- set instance = me.from_instance.name -*/
/*- set interface = me.from_interface.name -*/

/* Interface-specific error handling */
/*- set error_handler = c_symbol('error_handler') -*/
/*- include 'error-handler.c' -*/

/*# Conservative calculation of the numbers of threads in this component. #*/
/*- set threads = (1 if me.from_instance.type.control else 0) + len(me.from_instance.type.provides) + len(me.from_instance.type.uses) + len(me.from_instance.type.emits) + len(me.from_instance.type.consumes) -*/

/*- set userspace_buffer_ep = [None] -*/
/*- set userspace_buffer_sem_value = c_symbol() -*/
/*- if threads > 1 and userspace_ipc -*/
  /*# If we have more than one thread and we're using a userspace memory window
   *# in lieu of the IPC buffer, multiple threads can end up racing on accesses
   *# to this window. To prevent this, we use a lock built on an endpoint.
   #*/
  /*- do userspace_buffer_ep.__setitem__(0, alloc('userspace_buffer_ep', seL4_EndpointObject, write=True, read=True)) -*/
  static volatile int /*? userspace_buffer_sem_value ?*/ = 1;
/*- endif -*/
/*- set userspace_buffer_ep = userspace_buffer_ep[0] -*/

int /*? me.from_interface.name ?*/__run(void) {
    /* No setup required */
    return 0;
}

/*- for i, m in enumerate(me.from_interface.type.methods) -*/

/*- set name = m.name -*/
/*- set function = '%s_marshal' % m.name -*/
/*- set buffer = base -*/
/*- set sizes = [None] -*/
/*- if userspace_ipc -*/
    /*- do sizes.__setitem__(0, 'PAGE_SIZE_4K') -*/
/*- else -*/
    /*- do sizes.__setitem__(0, 'seL4_MsgMaxLength * sizeof(seL4_Word)') -*/
/*- endif -*/
/*- set size = sizes[0] -*/
/*- set method_index = i -*/
/*- set input_parameters = filter(lambda('x: x.direction.direction in [\'in\', \'inout\']'), m.parameters) -*/
/*- include 'marshal-inputs.c' -*/

/*- set function = '%s_unmarshal' % m.name -*/
/*- set output_parameters = filter(lambda('x: x.direction.direction in [\'out\', \'inout\']'), m.parameters) -*/
/*- set return_type = m.return_type -*/
/*- set allow_trailing_data = userspace_ipc -*/
/*- include 'unmarshal-outputs.c' -*/

/*- if m.return_type -*/
    /*? show(m.return_type) ?*/
/*- else -*/
    void
/*- endif -*/
/*? me.from_interface.name ?*/_/*? m.name ?*/(
/*- set ret_sz = c_symbol('ret_sz') -*/
/*- if m.return_type and m.return_type.array -*/
    size_t * /*? ret_sz ?*/
    /*- if len(m.parameters) > 0 -*/
        ,
    /*- endif -*/
/*- endif -*/
    /*? ', '.join(map(show, m.parameters)) ?*/
) {

    /*# The optimisation below is only valid to perform if we do not have any
     *# reference (typedefed C) types.
     #*/
    /*- set contains_reference_type = [False] -*/
    /*- for p in m.parameters -*/
      /*- if isinstance(p.type, camkes.ast.Reference) -*/
        /*- do contains_reference_type.__setitem__(0, True) -*/
        /*- break -*/
      /*- endif -*/
    /*- endfor -*/

    /*- if options.fspecialise_syscall_stubs and not contains_reference_type[0] and len(filter(lambda('x: x.array or x.type.type == \'string\''), m.parameters)) == 0 -*/
#ifdef ARCH_ARM
#ifndef __SWINUM
    #define __SWINUM(x) ((x) & 0x00ffffff)
#endif
        /*- if methods_len == 1 and not m.return_type and len(m.parameters) == 0 -*/
            /* We don't need to send or return any information because this
             * is the only method in this interface and it has no parameters or
             * return value. We can use an optimised syscall stub and take an
             * early exit.
             *
             * To explain where this stub deviates from the standard Call stub:
             *  - No asm clobbers because we're not receiving any arguments in
             *    the reply (that would usually clobber r2-r5);
             *  - Message info as an input only because we know the return info
             *    will be identical, so the compiler can avoid reloading it if
             *    we need the value after the syscall; and
             *  - Setup r7 and r1 first because they are preserved across the
             *    syscall and this helps the compiler emit a backwards branch
             *    to create a tight loop if we're calling this interface
             *    repeatedly.
             */
            /*- set scno = c_symbol() -*/
            register seL4_Word /*? scno ?*/ asm("r7") = seL4_SysCall;
            /*- set tag = c_symbol() -*/
            register seL4_MessageInfo_t /*? tag ?*/ asm("r1") = seL4_MessageInfo_new(0, 0, 0, 0);
            /*- set dest = c_symbol() -*/
            register seL4_Word /*? dest ?*/ asm("r0") = /*? ep ?*/;
            asm volatile("swi %[swinum]"
                /*- if trust_partner -*/
                    :"+r"(/*? dest ?*/)
                    :[swinum]"i"(__SWINUM(seL4_SysCall)), "r"(/*? scno ?*/), "r"(/*? tag ?*/)
                /*- else -*/
                    :"+r"(/*? dest ?*/), "r"(/*? tag ?*/)
                    :[swinum]"i"(__SWINUM(seL4_SysCall)), "r"(/*? scno ?*/)
                    :"r2", "r3", "r4", "r5", "memory"
                /*- endif -*/
            );
            return;
        /*- endif -*/
#endif
    /*- endif -*/

    /*# We're about to start writing to the buffer. If relevant, protect our
     *# access.
     #*/
    /*- if userspace_buffer_ep -*/
      sync_sem_bare_wait(/*? userspace_buffer_ep ?*/,
        &/*? userspace_buffer_sem_value ?*/);
    /*- endif -*/

    /* Marshal all the parameters */
    /*- set function = '%s_marshal' % m.name -*/
    /*- set length = c_symbol('length') -*/
    unsigned int /*? length ?*/ = /*- include 'call-marshal-inputs.c' -*/;
    if (unlikely(/*? length ?*/ == UINT_MAX)) {
        /* Error in marshalling; bail out. */
        /*- if m.return_type -*/
            /*- if m.return_type.array or (isinstance(m.return_type, camkes.ast.Type) and m.return_type.type == 'string')  -*/
                return NULL;
            /*- else -*/
                /*- set ret = c_symbol() -*/
                /*? show(m.return_type) ?*/ /*? ret ?*/;
                memset(& /*? ret ?*/, 0, sizeof(/*? ret ?*/));
                return /*? ret ?*/;
            /*- endif -*/
        /*- else -*/
            return;
        /*- endif -*/
    }

    /* Call the endpoint */
    /*- set info = c_symbol('info') -*/
    seL4_MessageInfo_t /*? info ?*/ = seL4_MessageInfo_new(0, 0, 0,
        /*- if userspace_ipc -*/
                0
        /*- else -*/
                ROUND_UP(/*? length ?*/, sizeof(seL4_Word)) / sizeof(seL4_Word)
        /*- endif -*/
        );
    /*? info ?*/ = seL4_Call(/*? ep ?*/, /*? info ?*/);

    /*- set size = c_symbol('size') -*/
    unsigned int /*? size ?*/ =
    /*- if userspace_ipc -*/
        /*? sizes[0] ?*/;
    /*- else -*/
        seL4_MessageInfo_get_length(/*? info ?*/) * sizeof(seL4_Word);
        assert(/*? size ?*/ <= seL4_MsgMaxLength * sizeof(seL4_Word));
    /*- endif -*/

    /* Unmarshal the response */
    /*- set function = '%s_unmarshal' % m.name -*/
    /*- set return_type = m.return_type -*/
    /*- set ret = c_symbol('return') -*/
    /*- if return_type -*/
      /*- if return_type.array -*/
        /*- if isinstance(return_type, camkes.ast.Type) and return_type.type == 'string' -*/
          char **
        /*- else -*/
          /*? show(return_type) ?*/ *
        /*- endif -*/
      /*- elif isinstance(return_type, camkes.ast.Type) and return_type.type == 'string' -*/
        char *
      /*- else -*/
        /*? show(return_type) ?*/
      /*- endif -*/
      /*? ret ?*/;
    /*- endif -*/
    /*- set err = c_symbol('error') -*/
    int /*? err ?*/ = /*- include 'call-unmarshal-outputs.c' -*/;
    if (unlikely(/*? err ?*/ != 0)) {
        /* Error in unmarshalling; bail out. */
        /*- if m.return_type -*/
            /*- if m.return_type.array or (isinstance(m.return_type, camkes.ast.Type) and m.return_type.type == 'string')  -*/
                return NULL;
            /*- else -*/
                memset(& /*? ret ?*/, 0, sizeof(/*? ret ?*/));
                return /*? ret ?*/;
            /*- endif -*/
        /*- else -*/
            return;
        /*- endif -*/
    }

    /*- if userspace_buffer_ep -*/
      sync_sem_bare_post(/*? userspace_buffer_ep ?*/,
        &/*? userspace_buffer_sem_value ?*/);
    /*- endif -*/

    /*- if m.return_type -*/
        return /*? ret ?*/;
    /*- endif -*/
}
/*- endfor -*/
