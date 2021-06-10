#include "stdafx.h"
#include "SbieModel.h"
#include "../../MiscHelpers/Common/Common.h"
#include "../../MiscHelpers/Common/IconExtreactor.h"
#include <QFileIconProvider>

CSbieModel::CSbieModel(QObject *parent)
:CTreeItemModel(parent)
{
	for (int i = 0; i < eMaxColor; i++)
		m_BoxIcons[(EBoxColors)i] = qMakePair(QIcon(QString(":/Boxes/Empty%1").arg(i)), QIcon(QString(":/Boxes/Full%1").arg(i)));

	//m_BoxEmpty = QIcon(":/BoxEmpty");
	//m_BoxInUse = QIcon(":/BoxInUse");
	m_ExeIcon = QIcon(":/exeIcon32");

	m_Root = MkNode(QVariant());
}

CSbieModel::~CSbieModel()
{
}

QList<QVariant> CSbieModel::MakeProcPath(const QString& BoxName, const CBoxedProcessPtr& pProcess, const QMap<quint32, CBoxedProcessPtr>& ProcessList)
{
	QList<QVariant> Path = MakeProcPath(pProcess, ProcessList);
	Path.prepend(BoxName);
	return Path;
}

QList<QVariant> CSbieModel::MakeProcPath(const CBoxedProcessPtr& pProcess, const QMap<quint32, CBoxedProcessPtr>& ProcessList)
{
	quint32 ParentID = pProcess->GetParendPID();
	CBoxedProcessPtr pParent = ProcessList.value(ParentID);

	QList<QVariant> Path;
	if (!pParent.isNull() && ParentID != pProcess->GetProcessId())
	{
		Path = MakeProcPath(pParent, ProcessList);
		Path.append(ParentID);
	}
	return Path;
}

bool CSbieModel::TestProcPath(const QList<QVariant>& Path, const QString& BoxName, const CBoxedProcessPtr& pProcess, const QMap<quint32, CBoxedProcessPtr>& ProcessList, int Index)
{
	if (Index == 0)
	{
		if (Path.isEmpty() || BoxName != Path[0])
			return false;

		return TestProcPath(Path, BoxName, pProcess, ProcessList, 1);
	}

	quint32 ParentID = pProcess->GetParendPID();
	CBoxedProcessPtr pParent = ProcessList.value(ParentID);

	if (!pParent.isNull() && ParentID != pProcess->GetProcessId())
	{
		if(Index >= Path.size() || Path[Path.size() - Index] != ParentID)
			return false;

		return TestProcPath(Path, BoxName, pParent, ProcessList, Index + 1);
	}

	return Path.size() == Index;
}

QString CSbieModel__AddGroupMark(const QString& Name)
{
	return Name.isEmpty() ? "" : ("!" + Name);
}

QString CSbieModel__RemoveGroupMark(const QString& Name)
{
	return Name.left(1) == "!" ? Name.mid(1) : Name;
}

QString CSbieModel::FindParent(const QVariant& Name, const QMap<QString, QStringList>& Groups)
{
	for(auto I = Groups.begin(); I != Groups.end(); ++I)
	{
		if (I.value().contains(CSbieModel__RemoveGroupMark(Name.toString()), Qt::CaseInsensitive))
			return CSbieModel__AddGroupMark(I.key());
	}
	return QString();
}

void CSbieModel::MakeBoxPath(const QVariant& Name, const QMap<QString, QStringList>& Groups, QList<QVariant>& Path)
{
	QString ParentID = FindParent(Name, Groups);

	if (!ParentID.isEmpty() && ParentID != Name && !Path.contains(ParentID))
	{
		Path.prepend(ParentID);
		MakeBoxPath(ParentID, Groups, Path);
	}
}

QList<QVariant>	CSbieModel::MakeBoxPath(const QVariant& Name, const QMap<QString, QStringList>& Groups)
{
	QList<QVariant> Path;
	MakeBoxPath(Name, Groups, Path);
	return Path;
}

QList<QVariant> CSbieModel::Sync(const QMap<QString, CSandBoxPtr>& BoxList, const QMap<QString, QStringList>& Groups, bool ShowHidden)
{
	QList<QVariant> Added;
	QMap<QList<QVariant>, QList<STreeNode*> > New;
	QHash<QVariant, STreeNode*> Old = m_Map;

	foreach(const QString& Group, Groups.keys())
	{
		if (Group.isEmpty())
			continue;
		QVariant ID = CSbieModel__AddGroupMark(Group);

		QHash<QVariant, STreeNode*>::iterator I = Old.find(ID);
		SSandBoxNode* pNode = I != Old.end() ? static_cast<SSandBoxNode*>(I.value()) : NULL;
		if (!pNode)
		{
			pNode = static_cast<SSandBoxNode*>(MkNode(ID));
			pNode->Values.resize(columnCount());
			if (m_bTree) 
				pNode->Path = MakeBoxPath(ID, Groups); 
			pNode->pBox = NULL;
			New[pNode->Path].append(pNode);
			Added.append(ID);

			pNode->Icon = m_BoxIcons[eYelow].first;
			pNode->IsBold = true;

			pNode->Values[eName].Raw = Group;
			pNode->Values[eStatus].Raw = tr("Box Groupe");
		}
		else
		{
			I.value() = NULL;
		}
	}

	foreach (const CSandBoxPtr& pBox, BoxList)
	{
		if (!ShowHidden && !pBox->IsEnabled())
			continue;

		QVariant ID = pBox->GetName();

		QModelIndex Index;
		
		QHash<QVariant, STreeNode*>::iterator I = Old.find(ID);
		SSandBoxNode* pNode = I != Old.end() ? static_cast<SSandBoxNode*>(I.value()) : NULL;
		if(!pNode)
		{
			pNode = static_cast<SSandBoxNode*>(MkNode(ID));
			pNode->Values.resize(columnCount());
			if (m_bTree)
				pNode->Path = MakeBoxPath(ID, Groups);
			pNode->pBox = pBox;
			New[pNode->Path].append(pNode);
			Added.append(ID);
		}
		else
		{
			I.value() = NULL;
			Index = Find(m_Root, pNode);
		}

		CSandBoxPlus* pBoxEx = qobject_cast<CSandBoxPlus*>(pBox.data());

		int Col = 0;
		bool State = false;
		int Changed = 0;

		QMap<quint32, CBoxedProcessPtr> ProcessList = pBox->GetProcessList();

		bool HasActive = Sync(pBox, pNode->Path, ProcessList, New, Old, Added);
		int inUse = (HasActive ? 1 : 0);
		int boxType = eYelow;
		if(pBoxEx->HasLogApi())
			boxType = eRed;
		if (pBoxEx->IsUnsecureDebugging())
			boxType = eMagenta;
		else if (pBoxEx->IsSecurityRestricted())
			boxType = eOrang;

		if (pNode->inUse != inUse || pNode->boxType != boxType)
		{
			pNode->inUse = inUse;
			pNode->boxType = boxType;
			//pNode->Icon = pNode->inUse ? m_BoxInUse : m_BoxEmpty;
			pNode->Icon = pNode->inUse ? m_BoxIcons[(EBoxColors)boxType].second : m_BoxIcons[(EBoxColors)boxType].first;
			Changed = 1; // set change for first column
		}

		if (pNode->IsGray != !pBoxEx->IsEnabled())
		{
			pNode->IsGray = !pBoxEx->IsEnabled();
			Changed = 2; // set change for all columns
		}

		for(int section = 0; section < columnCount(); section++)
		{
			if (!m_Columns.contains(section))
				continue; // ignore columns which are hidden

			QVariant Value;
			switch(section)
			{
				case eName:				Value = pBox->GetName(); break;
				case eStatus:			Value = pBox.objectCast<CSandBoxPlus>()->GetStatusStr(); break;
				case ePath:				Value = pBox->GetFileRoot(); break;
			}

			SSandBoxNode::SValue& ColValue = pNode->Values[section];

			if (ColValue.Raw != Value)
			{
				if(Changed == 0)
					Changed = 1;
				ColValue.Raw = Value;

				switch (section)
				{
				case eName:				ColValue.Formated = Value.toString().replace("_", " "); break;
				}
			}

			if(State != (Changed != 0))
			{
				if(State && Index.isValid())
					emit dataChanged(createIndex(Index.row(), Col, pNode), createIndex(Index.row(), section-1, pNode));
				State = (Changed != 0);
				Col = section;
			}
			if(Changed == 1)
				Changed = 0;
		}
		if(State && Index.isValid())
			emit dataChanged(createIndex(Index.row(), Col, pNode), createIndex(Index.row(), columnCount()-1, pNode));
	}

	CTreeItemModel::Sync(New, Old);
	return Added;
}

bool CSbieModel::Sync(const CSandBoxPtr& pBox, const QList<QVariant>& Path, const QMap<quint32, CBoxedProcessPtr>& ProcessList, QMap<QList<QVariant>, QList<STreeNode*> >& New, QHash<QVariant, STreeNode*>& Old, QList<QVariant>& Added)
{
	QString BoxName = pBox->GetName();

	int ActiveCount = 0;

	QFileIconProvider IconProvider;

	foreach(const CBoxedProcessPtr& pProc, ProcessList)
	{
		QSharedPointer<CSbieProcess> pProcess = pProc.objectCast<CSbieProcess>();
		QVariant ID = pProcess->GetProcessId();

		QModelIndex Index;

		QHash<QVariant, STreeNode*>::iterator I = Old.find(ID);
		SSandBoxNode* pNode = I != Old.end() ? static_cast<SSandBoxNode*>(I.value()) : NULL;
		if (!pNode || (m_bTree ? !TestProcPath(pNode->Path.mid(Path.length()), BoxName, pProcess, ProcessList) : !pNode->Path.isEmpty())) // todo: improve that
		{
			pNode = static_cast<SSandBoxNode*>(MkNode(ID));
			pNode->Values.resize(columnCount());
			if (m_bTree)
				pNode->Path = Path + MakeProcPath(BoxName, pProcess, ProcessList);
			pNode->pBox = pBox;
			pNode->pProcess = pProcess;
			New[pNode->Path].append(pNode);
			Added.append(ID);
		}
		else
		{
			I.value() = NULL;
			Index = Find(m_Root, pNode);
		}

		//if(Index.isValid()) // this is to slow, be more precise
		//	emit dataChanged(createIndex(Index.row(), 0, pNode), createIndex(Index.row(), columnCount()-1, pNode));

		int Col = 0;
		bool State = false;
		int Changed = 0;

		bool bIsTerminated = pProcess->IsTerminated();
		if (pNode->IsGray != bIsTerminated)
		{
			pNode->IsGray = bIsTerminated;
			Changed = 2; // update all columns for this item
		}

		if (!bIsTerminated)
			ActiveCount++;

		if (pNode->Icon.isNull())
		{
			//PixmapEntryList icons = extractIcons(pProcess->GetFileName(), false);
			//if (icons.isEmpty())
			//	pNode->Icon = m_ExeIcon;
			//else
			//	pNode->Icon = icons.first().pixmap;

			pNode->Icon = IconProvider.icon(QFileInfo(pProcess->GetFileName()));
			if (pNode->Icon.isNull() || !pNode->Icon.isValid())
				pNode->Icon = m_ExeIcon;
		}

		for (int section = 0; section < columnCount(); section++)
		{
			if (!m_Columns.contains(section))
				continue; // ignore columns which are hidden

			QVariant Value;
			switch (section)
			{
			case eName:				Value = pProcess->GetProcessName(); break;
			case eProcessId:		Value = pProcess->GetProcessId(); break;
			case eStatus:			Value = pProcess->GetStatusStr(); break;
			case eTitle:			Value = theAPI->GetProcessTitle(pProcess->GetProcessId()); break;
			//case eLogCount:			break; // todo Value = pProcess->GetResourceLog().count(); break;
			case eTimeStamp:		Value = pProcess->GetTimeStamp(); break;
			//case ePath:				Value = pProcess->GetFileName(); break;
			case ePath: {
									QString CmdLine = pProcess->GetCommandLine(); 
									Value = CmdLine.isEmpty() ? pProcess->GetFileName() : CmdLine;
									break;
						}
			}

			SSandBoxNode::SValue& ColValue = pNode->Values[section];

			if (ColValue.Raw != Value)
			{
				if (Changed == 0)
					Changed = 1;
				ColValue.Raw = Value;

				switch (section)
				{
					case eProcessId:		ColValue.Formated = QString::number(pProcess->GetProcessId()); break;
					//case eLogCount:			ColValue.Formated = QString::number(Value.toInt()); break;
					case eTimeStamp:		ColValue.Formated = pProcess->GetTimeStamp().toString("hh:mm:ss"); break;
				}
			}

			if (State != (Changed != 0))
			{
				if (State && Index.isValid())
					emit dataChanged(createIndex(Index.row(), Col, pNode), createIndex(Index.row(), section - 1, pNode));
				State = (Changed != 0);
				Col = section;
			}
			if (Changed == 1)
				Changed = 0;
		}
		if (State && Index.isValid())
			emit dataChanged(createIndex(Index.row(), Col, pNode), createIndex(Index.row(), columnCount() - 1, pNode));
	}

	return ActiveCount != 0;
}

CSandBoxPtr CSbieModel::GetSandBox(const QModelIndex &index) const
{
	if (!index.isValid())
        return CSandBoxPtr();

	SSandBoxNode* pNode = static_cast<SSandBoxNode*>(index.internalPointer());
	ASSERT(pNode);

	return pNode->pBox;
}

CBoxedProcessPtr CSbieModel::GetProcess(const QModelIndex &index) const
{
	if (!index.isValid())
		return CBoxedProcessPtr();

	SSandBoxNode* pNode = static_cast<SSandBoxNode*>(index.internalPointer());
	ASSERT(pNode);

	return pNode->pProcess;
}

QVariant CSbieModel::GetID(const QModelIndex &index) const
{
	if (!index.isValid())
		return QVariant();

	SSandBoxNode* pNode = static_cast<SSandBoxNode*>(index.internalPointer());
	ASSERT(pNode);

	if (!pNode->pProcess && !pNode->pBox)
		return CSbieModel__RemoveGroupMark(pNode->ID.toString());

	return pNode->ID;
}

CSbieModel::ETypes CSbieModel::GetType(const QModelIndex &index) const
{
	if (!index.isValid())
		return eNone;

	SSandBoxNode* pNode = static_cast<SSandBoxNode*>(index.internalPointer());
	ASSERT(pNode);

	if (pNode->pProcess)
		return eProcess;
	if (pNode->pBox)
		return eBox;
	return eGroup;
}

int CSbieModel::columnCount(const QModelIndex &parent) const
{
	return eCount;
}

QVariant CSbieModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation == Qt::Horizontal && role == Qt::DisplayRole)
	{
		switch(section)
		{
			case eName:				return tr("Name");
			case eProcessId:		return tr("Process ID");
			case eStatus:			return tr("Status");
			case eTitle:			return tr("Title");
			//case eLogCount:			return tr("Log Count");
			case eTimeStamp:		return tr("Start Time");
			case ePath:				return tr("Path / Command Line");
		}
	}
    return QVariant();
}

/*QVariant CSbieModel::GetDefaultIcon() const 
{ 
	return g_ExeIcon;
}*/
