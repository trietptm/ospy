//
// Copyright (c) 2007 Ole Andr� Vadla Ravn�s <oleavr@gmail.com>
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

#ifndef LOGGER_H
#define LOGGER_H

#include "Event.h"

#include <wdm.h>

#pragma warning(push)
#pragma warning(disable:4200)
#include <usbdi.h>
#pragma warning(pop)

#define MAX_PATH 260

typedef struct {
  WCHAR LogPath[MAX_PATH];
  volatile ULONG LogIndexUserspace;
  volatile ULONG LogCount;
  volatile ULONG LogSize;
} Capture;

typedef struct {
  SLIST_ENTRY entry;
  Event event;
} LogEntry;

class Logger
{
public:
  static void Initialize ();
  static void Shutdown ();

  NTSTATUS Start (IO_REMOVE_LOCK * removeLock, const WCHAR * fnSuffix);
  void Stop ();

  void LogUrb (const URB * urb);

private:
  static void LogThreadFuncWrapper (void * parameter) { static_cast <Logger *> (parameter)->LogThreadFunc (); }
  void LogThreadFunc ();

  void WriteNode (const Node * node);

  void WriteRaw (const void * data, size_t dataSize);
  void Write (ULONG dw);
  void Write (const char * str);

  static HANDLE m_captureSection;
  static Capture * m_capture;
  static volatile ULONG m_index;

  IO_REMOVE_LOCK * m_removeLock;

  HANDLE m_fileHandle;

  KEVENT m_stopEvent;
  HANDLE m_logThread;

  SLIST_HEADER m_items;
  KSPIN_LOCK m_itemsLock;
};

#endif // LOGGER_H