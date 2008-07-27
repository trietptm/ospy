//
// Copyright (c) 2007 Ole André Vadla Ravnås <oleavr@gmail.com>
//
// This library is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This library is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//

using System;
using System.Collections.Generic;
using System.Text;
using System.Threading;
using System.Collections.Specialized;
using System.ComponentModel;
using System.IO;
using System.Xml;

namespace oSpy.SharpDumpLib
{
    public delegate void SaveCompletedEventHandler(object sender, SaveCompletedEventArgs e);

    public class DumpSaver : AsyncWorker
    {
        #region Events

        public event ProgressChangedEventHandler SaveProgressChanged;
        public event SaveCompletedEventHandler SaveCompleted;

        #endregion // Events

        #region Internal members

        private delegate void WorkerEventHandler(Dump dump, Stream stream, AsyncOperation asyncOp, SendOrPostCallback completionMethodDelegate);
        private WorkerEventHandler workerDelegate;

        #endregion // Internal members

        #region Construction and destruction

        public DumpSaver()
            : base()
        {
        }

        public DumpSaver(IContainer container)
            : base(container)
        {
        }

        #endregion // Construction and destruction

        #region Public interface

        public virtual void Save(Dump dump, Stream stream)
        {
            DoSave(dump, stream, null);
        }

        public virtual void SaveAsync(Dump dump, Stream stream, object taskId)
        {
            AsyncOperation asyncOp = CreateOperation(taskId);

            workerDelegate = new WorkerEventHandler(SaveWorker);
            workerDelegate.BeginInvoke(dump, stream, asyncOp, completionMethodDelegate, null, null);
        }

        public virtual void SaveAsyncCancel(object taskId)
        {
            CancelOperation(taskId);
        }

        #endregion // Public interface

        #region Async glue

        private void SaveWorker(Dump dump, Stream stream, AsyncOperation asyncOp, SendOrPostCallback completionMethodDelegate)
        {
            Exception e = null;

            try
            {
                DoSave(dump, stream, asyncOp);
            }
            catch (Exception ex)
            {
                e = ex;
            }

            SaveState saveState = new SaveState(dump, stream, e, asyncOp);

            try { completionMethodDelegate(saveState); }
            catch (InvalidOperationException) { }
        }

        protected override object CreateCancelEventArgs(object userSuppliedState)
        {
            return new SaveCompletedEventArgs(null, null, null, true, userSuppliedState);
        }

        protected override void ReportProgress(object e)
        {
            OnSaveProgressChanged(e as ProgressChangedEventArgs);
        }

        protected virtual void OnSaveProgressChanged(ProgressChangedEventArgs e)
        {
            if (SaveProgressChanged != null)
                SaveProgressChanged(this, e);
        }

        protected override void ReportCompletion(object e)
        {
            OnSaveCompleted(e as SaveCompletedEventArgs);
        }

        protected virtual void OnSaveCompleted(SaveCompletedEventArgs e)
        {
            if (SaveCompleted != null)
                SaveCompleted(this, e);
        }

        protected override void CompletionMethod(object state)
        {
            SaveState saveState = state as SaveState;

            AsyncOperation asyncOp = saveState.asyncOp;
            SaveCompletedEventArgs e = new SaveCompletedEventArgs(saveState.dump, saveState.stream, saveState.ex, false, asyncOp.UserSuppliedState);
            FinalizeOperation(asyncOp, e);
        }

        #endregion // Async glue

        #region Core implementation

        private void DoSave(Dump dump, Stream stream, AsyncOperation asyncOp)
        {
            BinaryWriter binWriter = new BinaryWriter(stream, Encoding.UTF8);
            binWriter.Write((uint)dump.Events.Count);
            binWriter.Flush();
            binWriter = null;

            XmlTextWriter xmlWriter = new XmlTextWriter(stream, Encoding.UTF8);
            xmlWriter.WriteStartDocument(true);
            xmlWriter.WriteStartElement("events");

            int n = 0;
            int numEvents = dump.Events.Count;
            foreach (Event ev in dump.Events.Values)
            {
                if (asyncOp != null)
                {
                    int pctComplete = (int)(((float)(n + 1) / (float)numEvents) * 100.0f);
                    ProgressChangedEventArgs e = new ProgressChangedEventArgs(pctComplete, asyncOp.UserSuppliedState);
                    asyncOp.Post(onProgressReportDelegate, e);
                }

                xmlWriter.WriteRaw(ev.RawData);

                n++;
            }

            xmlWriter.WriteEndElement();
            xmlWriter.Flush();
        }

        #endregion // Core implementation
    }

    #region Helper classes

    internal class SaveState
    {
        public Dump dump = null;
        public Stream stream = null;
        public Exception ex = null;
        public AsyncOperation asyncOp = null;

        public SaveState(Dump dump, Stream stream, Exception ex, AsyncOperation asyncOp)
        {
            this.dump = dump;
            this.stream = stream;
            this.ex = ex;
            this.asyncOp = asyncOp;
        }
    }

    public class SaveCompletedEventArgs : AsyncCompletedEventArgs
    {
        private Dump dump = null;
        private Stream stream = null;

        public SaveCompletedEventArgs(Dump dump, Stream stream, Exception e, bool cancelled, object state)
            : base(e, cancelled, state)
        {
            this.dump = dump;
            this.stream = stream;
        }

        public Dump Dump
        {
            get
            {
                RaiseExceptionIfNecessary();

                return dump;
            }
        }

        public Stream Stream
        {
            get
            {
                RaiseExceptionIfNecessary();

                return stream;
            }
        }
    }

    #endregion // Helper classes
}