/*
 *
 * Copyright (c) 2020, David Xanatos
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
#pragma once
#include <qobject.h>

#include "../qsbieapi_global.h"

#include "BoxedProcess.h"
#include "SbieIni.h"

struct QSBIEAPI_EXPORT SBoxSnapshot
{
	QString ID;
	QString Parent;

	QString NameStr;
	QString InfoStr;
	QDateTime SnapDate;
};

class QSBIEAPI_EXPORT CSandBox : public CSbieIni
{
	Q_OBJECT
public:
	CSandBox(const QString& BoxName, class CSbieAPI* pAPI);
	virtual ~CSandBox();

	virtual void					UpdateDetails();

	virtual QString					GetFileRoot() const { return m_FilePath; }
	virtual QString					GetRegRoot() const { return m_RegPath; }
	virtual QString					GetIpcRoot() const { return m_IpcPath; }

	virtual QMap<quint32, CBoxedProcessPtr>	GetProcessList() const { return m_ProcessList; }

	virtual int						GetActiveProcessCount() const { return m_ActiveProcessCount; }

	virtual SB_STATUS				RunStart(const QString& Command, bool Elevated = false);
	virtual SB_STATUS				RunSandboxed(const QString& Command);
	virtual SB_STATUS				TerminateAll();

	virtual void					CloseBox() {}

	virtual bool					IsEnabled() const  { return m_IsEnabled; }

	virtual bool					IsEmpty() const;
	virtual SB_PROGRESS				CleanBox();
	virtual SB_STATUS				RenameBox(const QString& NewName);
	virtual SB_STATUS				RemoveBox();

	virtual QList<SBoxSnapshot>		GetSnapshots(QString* pCurrent = NULL) const;
	virtual SB_PROGRESS				TakeSnapshot(const QString& Name);
	virtual SB_PROGRESS				RemoveSnapshot(const QString& ID);
	virtual SB_PROGRESS				SelectSnapshot(const QString& ID);
	virtual SB_STATUS				SetSnapshotInfo(const QString& ID, const QString& Name, const QString& Description = QString());

	class CSbieAPI*					Api() { return m_pAPI; }

protected:
	friend class CSbieAPI;

	SB_PROGRESS						CleanBoxFolders(const QStringList& BoxFolders);
	static void						CleanBoxAsync(const CSbieProgressPtr& pProgress, const QStringList& BoxFolders);

	static void						DeleteSnapshotAsync(const CSbieProgressPtr& pProgress, const QString& BoxPath, const QString& ID);
	static void						MergeSnapshotAsync(const CSbieProgressPtr& pProgress, const QString& BoxPath, const QString& TargetID, const QString& SourceID);

	QString							m_FilePath;
	QString							m_RegPath;
	QString							m_IpcPath;
	
	bool							m_IsEnabled;
	
	QMap<quint32, CBoxedProcessPtr>	m_ProcessList;
	int								m_ActiveProcessCount;

//private:
//	struct SSandBox* m;
};

typedef QSharedPointer<CSandBox> CSandBoxPtr;
typedef QWeakPointer<CSandBox> CSandBoxRef;