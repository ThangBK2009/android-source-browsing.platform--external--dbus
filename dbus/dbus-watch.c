/* -*- mode: C; c-file-style: "gnu" -*- */
/* dbus-watch.c DBusWatch implementation
 *
 * Copyright (C) 2002, 2003  Red Hat Inc.
 *
 * Licensed under the Academic Free License version 1.2
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "dbus-internals.h"
#include "dbus-watch.h"
#include "dbus-list.h"

/**
 * @defgroup DBusWatchInternals DBusWatch implementation details
 * @ingroup  DBusInternals
 * @brief implementation details for DBusWatch
 * 
 * @{
 */

struct DBusWatch
{
  int refcount;                        /**< Reference count */
  int fd;                              /**< File descriptor. */
  unsigned int flags;                  /**< Conditions to watch. */
  void *data;                          /**< Application data. */
  DBusFreeFunction free_data_function; /**< Free the application data. */
  unsigned int enabled : 1;            /**< Whether it's enabled. */
};

/**
 * Creates a new DBusWatch. Normally used by a DBusTransport
 * implementation.
 * @param fd the file descriptor to be watched.
 * @param flags the conditions to watch for on the descriptor.
 * @param enabled the initial enabled state
 * @returns the new DBusWatch object.
 */
DBusWatch*
_dbus_watch_new (int          fd,
                 unsigned int flags,
                 dbus_bool_t  enabled)
{
  DBusWatch *watch;

#define VALID_WATCH_FLAGS (DBUS_WATCH_WRITABLE | DBUS_WATCH_READABLE)
  
  _dbus_assert ((flags & VALID_WATCH_FLAGS) == flags);
  
  watch = dbus_new0 (DBusWatch, 1);
  watch->refcount = 1;
  watch->fd = fd;
  watch->flags = flags;
  watch->enabled = enabled;

  return watch;
}

/**
 * Increments the reference count of a DBusWatch object.
 *
 * @param watch the watch object.
 */
void
_dbus_watch_ref (DBusWatch *watch)
{
  watch->refcount += 1;
}

/**
 * Decrements the reference count of a DBusWatch object
 * and finalizes the object if the count reaches zero.
 *
 * @param watch the watch object.
 */
void
_dbus_watch_unref (DBusWatch *watch)
{
  _dbus_assert (watch != NULL);
  _dbus_assert (watch->refcount > 0);

  watch->refcount -= 1;
  if (watch->refcount == 0)
    {
      dbus_watch_set_data (watch, NULL, NULL); /* call free_data_function */
      dbus_free (watch);
    }
}

/**
 * Clears the file descriptor from a now-invalid watch object so that
 * no one tries to use it.  This is because a watch may stay alive due
 * to reference counts after the file descriptor is closed.
 * Invalidation makes it easier to catch bugs. It also
 * keeps people from doing dorky things like assuming file descriptors
 * are unique (never recycled).
 *
 * @param watch the watch object.
 */
void
_dbus_watch_invalidate (DBusWatch *watch)
{
  watch->fd = -1;
  watch->flags = 0;
}

/**
 * Sanitizes the given condition so that it only contains
 * flags that the DBusWatch requested. e.g. if the
 * watch is a DBUS_WATCH_READABLE watch then
 * DBUS_WATCH_WRITABLE will be stripped from the condition.
 *
 * @param watch the watch object.
 * @param condition address of the condition to sanitize.
 */
void
_dbus_watch_sanitize_condition (DBusWatch    *watch,
                                unsigned int *condition)
{
  if (!(watch->flags & DBUS_WATCH_READABLE))
    *condition &= ~DBUS_WATCH_READABLE;
  if (!(watch->flags & DBUS_WATCH_WRITABLE))
    *condition &= ~DBUS_WATCH_WRITABLE;
}


/**
 * @typedef DBusWatchList
 *
 * Opaque data type representing a list of watches
 * and a set of DBusAddWatchFunction/DBusRemoveWatchFunction.
 * Automatically handles removing/re-adding watches
 * when the DBusAddWatchFunction is updated or changed.
 * Holds a reference count to each watch.
 *
 * Used in the implementation of both DBusServer and
 * DBusClient.
 *
 */

/**
 * DBusWatchList implementation details. All fields
 * are private.
 *
 */
struct DBusWatchList
{
  DBusList *watches;           /**< Watch objects. */

  DBusAddWatchFunction add_watch_function;    /**< Callback for adding a watch. */
  DBusRemoveWatchFunction remove_watch_function; /**< Callback for removing a watch. */
  DBusWatchToggledFunction watch_toggled_function; /**< Callback on toggling enablement */
  void *watch_data;                           /**< Data for watch callbacks */
  DBusFreeFunction watch_free_data_function;  /**< Free function for watch callback data */
};

/**
 * Creates a new watch list. Returns #NULL if insufficient
 * memory exists.
 *
 * @returns the new watch list, or #NULL on failure.
 */
DBusWatchList*
_dbus_watch_list_new (void)
{
  DBusWatchList *watch_list;

  watch_list = dbus_new0 (DBusWatchList, 1);
  if (watch_list == NULL)
    return NULL;

  return watch_list;
}

/**
 * Frees a DBusWatchList.
 *
 * @param watch_list the watch list.
 */
void
_dbus_watch_list_free (DBusWatchList *watch_list)
{
  /* free watch_data and removes watches as a side effect */
  _dbus_watch_list_set_functions (watch_list,
                                  NULL, NULL, NULL, NULL, NULL);
  
  _dbus_list_foreach (&watch_list->watches,
                      (DBusForeachFunction) _dbus_watch_unref,
                      NULL);
  _dbus_list_clear (&watch_list->watches);

  dbus_free (watch_list);
}

/**
 * Sets the watch functions. This function is the "backend"
 * for dbus_connection_set_watch_functions() and
 * dbus_server_set_watch_functions().
 *
 * @param watch_list the watch list.
 * @param add_function the add watch function.
 * @param remove_function the remove watch function.
 * @param toggled_function function on toggling enabled flag, or #NULL
 * @param data the data for those functions.
 * @param free_data_function the function to free the data.
 * @returns #FALSE if not enough memory
 *
 */
dbus_bool_t
_dbus_watch_list_set_functions (DBusWatchList           *watch_list,
                                DBusAddWatchFunction     add_function,
                                DBusRemoveWatchFunction  remove_function,
                                DBusWatchToggledFunction toggled_function,
                                void                    *data,
                                DBusFreeFunction         free_data_function)
{
  /* Add watches with the new watch function, failing on OOM */
  if (add_function != NULL)
    {
      DBusList *link;
      
      link = _dbus_list_get_first_link (&watch_list->watches);
      while (link != NULL)
        {
          DBusList *next = _dbus_list_get_next_link (&watch_list->watches,
                                                     link);
      
          if (!(* add_function) (link->data, data))
            {
              /* remove it all again and return FALSE */
              DBusList *link2;
              
              link2 = _dbus_list_get_first_link (&watch_list->watches);
              while (link2 != link)
                {
                  DBusList *next = _dbus_list_get_next_link (&watch_list->watches,
                                                             link2);

                  (* remove_function) (link2->data, data);
                  
                  link2 = next;
                }

              return FALSE;
            }
      
          link = next;
        }
    }
  
  /* Remove all current watches from previous watch handlers */

  if (watch_list->remove_watch_function != NULL)
    {
      _dbus_list_foreach (&watch_list->watches,
                          (DBusForeachFunction) watch_list->remove_watch_function,
                          watch_list->watch_data);
    }

  if (watch_list->watch_free_data_function != NULL)
    (* watch_list->watch_free_data_function) (watch_list->watch_data);
  
  watch_list->add_watch_function = add_function;
  watch_list->remove_watch_function = remove_function;
  watch_list->watch_toggled_function = toggled_function;
  watch_list->watch_data = data;
  watch_list->watch_free_data_function = free_data_function;

  return TRUE;
}

/**
 * Adds a new watch to the watch list, invoking the
 * application DBusAddWatchFunction if appropriate.
 *
 * @param watch_list the watch list.
 * @param watch the watch to add.
 * @returns #TRUE on success, #FALSE if no memory.
 */
dbus_bool_t
_dbus_watch_list_add_watch (DBusWatchList *watch_list,
                            DBusWatch     *watch)
{
  if (!_dbus_list_append (&watch_list->watches, watch))
    return FALSE;
  
  _dbus_watch_ref (watch);

  if (watch_list->add_watch_function != NULL)
    {
      if (!(* watch_list->add_watch_function) (watch,
                                               watch_list->watch_data))
        {
          _dbus_list_remove_last (&watch_list->watches, watch);
          _dbus_watch_unref (watch);
          return FALSE;
        }
    }
  
  return TRUE;
}

/**
 * Removes a watch from the watch list, invoking the
 * application's DBusRemoveWatchFunction if appropriate.
 *
 * @param watch_list the watch list.
 * @param watch the watch to remove.
 */
void
_dbus_watch_list_remove_watch  (DBusWatchList *watch_list,
                                DBusWatch     *watch)
{
  if (!_dbus_list_remove (&watch_list->watches, watch))
    _dbus_assert_not_reached ("Nonexistent watch was removed");

  if (watch_list->remove_watch_function != NULL)
    (* watch_list->remove_watch_function) (watch,
                                           watch_list->watch_data);
  
  _dbus_watch_unref (watch);
}

/**
 * Sets a watch to the given enabled state, invoking the
 * application's DBusWatchToggledFunction if appropriate.
 *
 * @param watch_list the watch list.
 * @param watch the watch to toggle.
 * @param enabled #TRUE to enable
 */
void
_dbus_watch_list_toggle_watch (DBusWatchList           *watch_list,
                               DBusWatch               *watch,
                               dbus_bool_t              enabled)
{
  enabled = !!enabled;
  
  if (enabled == watch->enabled)
    return;

  watch->enabled = enabled;
  
  if (watch_list->watch_toggled_function != NULL)
    (* watch_list->watch_toggled_function) (watch,
                                            watch_list->watch_data);
}

/** @} */

/**
 * @defgroup DBusWatch DBusWatch
 * @ingroup  DBus
 * @brief Object representing a file descriptor to be watched.
 *
 * Types and functions related to DBusWatch. A watch represents
 * a file descriptor that the main loop needs to monitor,
 * as in Qt's QSocketNotifier or GLib's g_io_add_watch().
 * 
 * @{
 */

/**
 * @typedef DBusWatch
 *
 * Opaque object representing a file descriptor
 * to be watched for changes in readability,
 * writability, or hangup.
 */

/**
 * Gets the file descriptor that should be watched.
 *
 * @param watch the DBusWatch object.
 * @returns the file descriptor to watch.
 */
int
dbus_watch_get_fd (DBusWatch *watch)
{
  return watch->fd;
}

/**
 * Gets flags from DBusWatchFlags indicating
 * what conditions should be monitored on the
 * file descriptor.
 * 
 * The flags returned will only contain DBUS_WATCH_READABLE
 * and DBUS_WATCH_WRITABLE, never DBUS_WATCH_HANGUP or
 * DBUS_WATCH_ERROR; all watches implicitly include a watch
 * for hangups, errors, and other exceptional conditions.
 *
 * @param watch the DBusWatch object.
 * @returns the conditions to watch.
 */
unsigned int
dbus_watch_get_flags (DBusWatch *watch)
{
  _dbus_assert ((watch->flags & VALID_WATCH_FLAGS) == watch->flags);

  return watch->flags;
}

/**
 * Gets data previously set with dbus_watch_set_data()
 * or #NULL if none.
 *
 * @param watch the DBusWatch object.
 * @returns previously-set data.
 */
void*
dbus_watch_get_data (DBusWatch *watch)
{
  return watch->data;
}

/**
 * Sets data which can be retrieved with dbus_watch_get_data().
 * Intended for use by the DBusAddWatchFunction and
 * DBusRemoveWatchFunction to store their own data.  For example with
 * Qt you might store the QSocketNotifier for this watch and with GLib
 * you might store a GSource.
 *
 * @param watch the DBusWatch object.
 * @param data the data.
 * @param free_data_function function to be called to free the data.
 */
void
dbus_watch_set_data (DBusWatch        *watch,
                     void             *data,
                     DBusFreeFunction  free_data_function)
{
  if (watch->free_data_function != NULL)
    (* watch->free_data_function) (watch->data);
  
  watch->data = data;
  watch->free_data_function = free_data_function;
}

/**
 * Returns whether a watch is enabled or not. If not
 * enabled, it should not be polled by the main loop.
 *
 * @param watch the DBusWatch object
 * @returns #TRUE if the watch is enabled
 */
dbus_bool_t
dbus_watch_get_enabled (DBusWatch *watch)
{
  return watch->enabled;
}

/** @} */
