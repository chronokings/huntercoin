#include "managenamespage.h"
#include "ui_managenamespage.h"

#include "walletmodel.h"
#include "nametablemodel.h"
#include "csvmodelwriter.h"
#include "guiutil.h"
#include "../base58.h"
#include "../main.h"
#include "../hook.h"
#include "../wallet.h"
#include "../chronokings.h"
#include "guiconstants.h"
#include "ui_interface.h"
#include "configurenamedialog.h"

#include <QSortFilterProxyModel>
#include <QMessageBox>
#include <QMenu>
#include <QScrollBar>

extern std::map<std::vector<unsigned char>, PreparedNameFirstUpdate> mapMyNameFirstUpdate;

const QString STR_NAME_FIRSTUPDATE_DEFAULT = "{\"color\":0}";
const QString NON_REWARD_ADDRESS_PREFIX = "-";    // Prefix that indicates for the user that the player's reward address isn't the one shown

//
// NameFilterProxyModel
//

NameFilterProxyModel::NameFilterProxyModel(QObject *parent /*= 0*/)
    : QSortFilterProxyModel(parent)
{
}

void NameFilterProxyModel::setNameSearch(const QString &search)
{
    nameSearch = search;
    invalidateFilter();
}

void NameFilterProxyModel::setValueSearch(const QString &search)
{
    valueSearch = search;
    invalidateFilter();
}

void NameFilterProxyModel::setAddressSearch(const QString &search)
{
    addressSearch = search;
    invalidateFilter();
}

void NameFilterProxyModel::setStateSearch(const QString &search)
{
    stateSearch = search;
    invalidateFilter();
}

bool NameFilterProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    QModelIndex index = sourceModel()->index(sourceRow, 0, sourceParent);

    QString name = index.sibling(index.row(), NameTableModel::Name).data(Qt::EditRole).toString();
    QString value = index.sibling(index.row(), NameTableModel::Value).data(Qt::EditRole).toString();
    QString address = index.sibling(index.row(), NameTableModel::Address).data(Qt::EditRole).toString();
    QString state = index.sibling(index.row(), NameTableModel::State).data(Qt::EditRole).toString();

    Qt::CaseSensitivity case_sens = filterCaseSensitivity();
    return name.contains(nameSearch, case_sens)
        && value.contains(valueSearch, case_sens)
        && (address.startsWith(addressSearch, Qt::CaseSensitive)
               || (address.startsWith(NON_REWARD_ADDRESS_PREFIX)
                      && address.mid(NON_REWARD_ADDRESS_PREFIX.size()).startsWith(addressSearch, Qt::CaseSensitive)
                  )
           )
        && state.contains(stateSearch, case_sens);
}

//
// ManageNamesPage
//

const static int COLUMN_WIDTH_NAME = 300,
                 COLUMN_WIDTH_ADDRESS = 256,
                 COLUMN_WIDTH_STATUS = 100;

ManageNamesPage::ManageNamesPage(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::ManageNamesPage),
    model(0),
    walletModel(0),
    proxyModel(0)
{
    ui->setupUi(this);

    // Context menu actions
    QAction *copyNameAction = new QAction(tr("Copy &Name"), this);
    QAction *copyValueAction = new QAction(tr("Copy &Value"), this);
    QAction *copyAddressAction = new QAction(tr("Copy &Address"), this);
    QAction *copyStateAction = new QAction(tr("Copy &State"), this);
    QAction *configureNameAction = new QAction(tr("&Configure Name..."), this);

    // Build context menu
    contextMenu = new QMenu();
    contextMenu->addAction(copyNameAction);
    contextMenu->addAction(copyValueAction);
    contextMenu->addAction(copyAddressAction);
    contextMenu->addAction(copyStateAction);
    contextMenu->addAction(configureNameAction);

    // Connect signals for context menu actions
    connect(copyNameAction, SIGNAL(triggered()), this, SLOT(onCopyNameAction()));
    connect(copyValueAction, SIGNAL(triggered()), this, SLOT(onCopyValueAction()));
    connect(copyAddressAction, SIGNAL(triggered()), this, SLOT(onCopyAddressAction()));
    connect(copyStateAction, SIGNAL(triggered()), this, SLOT(onCopyStateAction()));
    connect(configureNameAction, SIGNAL(triggered()), this, SLOT(on_configureNameButton_clicked()));

    connect(ui->tableView, SIGNAL(customContextMenuRequested(QPoint)), this, SLOT(contextualMenu(QPoint)));
    connect(ui->tableView, SIGNAL(doubleClicked(QModelIndex)), this, SLOT(on_configureNameButton_clicked()));
    ui->tableView->setEditTriggers(QAbstractItemView::NoEditTriggers);

    // Catch focus changes to make the appropriate button the default one (Submit or Configure)
    ui->registerName->installEventFilter(this);
    ui->submitNameButton->installEventFilter(this);
    ui->tableView->installEventFilter(this);
    ui->nameFilter->installEventFilter(this);
    ui->valueFilter->installEventFilter(this);
    ui->addressFilter->installEventFilter(this);
    ui->stateFilter->installEventFilter(this);
    ui->configureNameButton->installEventFilter(this);

    ui->registerName->setMaxLength(MAX_NAME_LENGTH);

    ui->nameFilter->setMaxLength(MAX_NAME_LENGTH);
    ui->valueFilter->setMaxLength(MAX_VALUE_LENGTH);
    GUIUtil::setupAddressWidget(ui->addressFilter, this, true);

#if QT_VERSION >= 0x040700
    /* Do not move this to the XML file, Qt before 4.7 will choke on it */
    ui->nameFilter->setPlaceholderText(tr("Name filter"));
    ui->valueFilter->setPlaceholderText(tr("Value filter"));
    ui->addressFilter->setPlaceholderText(tr("Address filter"));
    ui->stateFilter->setPlaceholderText(tr("State filter"));
#endif

    ui->nameFilter->setFixedWidth(COLUMN_WIDTH_NAME);
    ui->addressFilter->setFixedWidth(COLUMN_WIDTH_ADDRESS);
    ui->horizontalSpacer_Status->changeSize(
        COLUMN_WIDTH_STATUS + ui->tableView->verticalScrollBar()->sizeHint().width()
            
#ifdef Q_OS_MAC
        // Not sure if this is needed, but other Mac code adds 2 pixels to scroll bar width;
        // see transactionview.cpp, search for verticalScrollBar()->sizeHint()
        + 2
#endif

        ,
        ui->horizontalSpacer_Status->sizeHint().height(),
        QSizePolicy::Fixed);
}

ManageNamesPage::~ManageNamesPage()
{
    delete ui;
}

void ManageNamesPage::setModel(WalletModel *walletModel)
{
    this->walletModel = walletModel;
    model = walletModel->getNameTableModel();

    proxyModel = new NameFilterProxyModel(this);
    proxyModel->setSourceModel(model);
    proxyModel->setDynamicSortFilter(true);
    proxyModel->setSortCaseSensitivity(Qt::CaseInsensitive);
    proxyModel->setFilterCaseSensitivity(Qt::CaseInsensitive);

    ui->tableView->setModel(proxyModel);
    ui->tableView->sortByColumn(0, Qt::AscendingOrder);

    ui->tableView->horizontalHeader()->setHighlightSections(false);

    // Set column widths
    ui->tableView->horizontalHeader()->resizeSection(
            NameTableModel::Name, COLUMN_WIDTH_NAME);
    ui->tableView->horizontalHeader()->setResizeMode(
            NameTableModel::Value, QHeaderView::Stretch);
    ui->tableView->horizontalHeader()->resizeSection(
            NameTableModel::Address, COLUMN_WIDTH_ADDRESS);
    ui->tableView->horizontalHeader()->setResizeMode(
            NameTableModel::State, QHeaderView::Stretch);
    ui->tableView->horizontalHeader()->resizeSection(
            NameTableModel::Status, COLUMN_WIDTH_STATUS);

    connect(ui->tableView->selectionModel(), SIGNAL(selectionChanged(QItemSelection,QItemSelection)),
            this, SLOT(selectionChanged()));
            
    connect(ui->nameFilter, SIGNAL(textChanged(QString)), this, SLOT(changedNameFilter(QString)));
    connect(ui->valueFilter, SIGNAL(textChanged(QString)), this, SLOT(changedValueFilter(QString)));
    connect(ui->addressFilter, SIGNAL(textChanged(QString)), this, SLOT(changedAddressFilter(QString)));
    connect(ui->stateFilter, SIGNAL(textChanged(QString)), this, SLOT(changedStateFilter(QString)));

    selectionChanged();
}

void ManageNamesPage::changedNameFilter(const QString &filter)
{
    if (!proxyModel)
        return;
    proxyModel->setNameSearch(filter);
}

void ManageNamesPage::changedValueFilter(const QString &filter)
{
    if (!proxyModel)
        return;
    proxyModel->setValueSearch(filter);
}

void ManageNamesPage::changedAddressFilter(const QString &filter)
{
    if (!proxyModel)
        return;
    proxyModel->setAddressSearch(filter);
}

void ManageNamesPage::changedStateFilter(const QString &filter)
{
    if (!proxyModel)
        return;
    proxyModel->setStateSearch(filter);
}

extern bool IsValidPlayerName(const std::string &);

void ManageNamesPage::on_submitNameButton_clicked()
{
    if (!walletModel)
        return;

    QString name = ui->registerName->text();

    if (!walletModel->nameAvailable(name))
    {
        QMessageBox::warning(this, tr("Name registration"), tr("Name not available"));
        ui->registerName->setFocus();
        return;
    }

    if (!IsValidPlayerName(name.toStdString()))
    {
        QMessageBox::warning(this, tr("Name registration error"),
              tr("The entered name is invalid. Allowed characters: alphanumeric, underscore, hyphen, whitespace (but not at start/end and no double whitespaces)."),
              QMessageBox::Ok);
        return;
    }

    if (QMessageBox::Yes != QMessageBox::question(this, tr("Confirm name registration"),
          tr("Are you sure you want to create player %1?").arg(name),
          QMessageBox::Yes | QMessageBox::Cancel,
          QMessageBox::Cancel))
    {
        return;
    }

    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return;
    
    QString err_msg;

    try
    {
        WalletModel::NameNewReturn res = walletModel->nameNew(name);

        if (res.ok)
        {
            ui->registerName->setText("");
            ui->submitNameButton->setDefault(true);

            int newRowIndex;
            // FIXME: CT_NEW may have been sent from nameNew (via transaction).
            // Currently updateEntry is modified so it does not complain
            model->updateEntry(name, STR_NAME_FIRSTUPDATE_DEFAULT, res.address, NameTableEntry::NAME_NEW, CT_NEW, &newRowIndex);
            ui->tableView->selectRow(newRowIndex);
            ui->tableView->setFocus();

            ConfigureNameDialog dlg(name, STR_NAME_FIRSTUPDATE_DEFAULT, res.address, true, this);
            dlg.setModel(walletModel);
            if (dlg.exec() == QDialog::Accepted)
            {
                LOCK(cs_main);
                if (mapMyNameFirstUpdate.count(vchFromString(name.toStdString())) != 0)
                    model->updateEntry(name, dlg.getReturnData(), res.address, NameTableEntry::NAME_NEW, CT_UPDATED);
                else
                {
                    // name_firstupdate could have been sent, while the user was editing the value
                    // Do nothing
                }
            }

            return;
        }

        err_msg = res.err_msg;
    }
    catch (std::exception& e) 
    {
        err_msg = e.what();
    }

    if (err_msg == "ABORTED")
        return;

    QMessageBox::warning(this, tr("Name registration failed"), err_msg);
}

bool ManageNamesPage::eventFilter(QObject *object, QEvent *event)
{
    if (event->type() == QEvent::FocusIn)
    {
        if (object == ui->registerName || object == ui->submitNameButton)
        {
            ui->submitNameButton->setDefault(true);
            ui->configureNameButton->setDefault(false);
        }
        else if (object == ui->tableView)
        {
            ui->submitNameButton->setDefault(false);
            ui->configureNameButton->setDefault(true);
        }
    }
    return QDialog::eventFilter(object, event);
}

void ManageNamesPage::selectionChanged()
{
    // Set button states based on selected tab and selection
    QTableView *table = ui->tableView;
    if(!table->selectionModel())
        return;

    ui->configureNameButton->setEnabled(table->selectionModel()->hasSelection());
}

void ManageNamesPage::contextualMenu(const QPoint &point)
{
    QModelIndex index = ui->tableView->indexAt(point);
    if (index.isValid())
        contextMenu->exec(QCursor::pos());
}

void ManageNamesPage::onCopyNameAction()
{
    GUIUtil::copyEntryData(ui->tableView, NameTableModel::Name);
}

void ManageNamesPage::onCopyValueAction()
{
    GUIUtil::copyEntryData(ui->tableView, NameTableModel::Value);
}

void ManageNamesPage::onCopyAddressAction()
{
    GUIUtil::copyEntryData(ui->tableView, NameTableModel::Address);
}

void ManageNamesPage::onCopyStateAction()
{
    GUIUtil::copyEntryData(ui->tableView, NameTableModel::State);
}

void ManageNamesPage::on_configureNameButton_clicked()
{
    if(!ui->tableView->selectionModel())
        return;
    QModelIndexList indexes = ui->tableView->selectionModel()->selectedRows(NameTableModel::Name);
    if(indexes.isEmpty())
        return;

    QModelIndex index = indexes.at(0);

    QString name = index.data(Qt::EditRole).toString();
    QString value = index.sibling(index.row(), NameTableModel::Value).data(Qt::EditRole).toString();
    QString address = index.sibling(index.row(), NameTableModel::Address).data(Qt::EditRole).toString();

    if (address.startsWith(NON_REWARD_ADDRESS_PREFIX))
        address = address.mid(NON_REWARD_ADDRESS_PREFIX.size());

    std::vector<unsigned char> vchName = vchFromString(name.toStdString());

    bool fFirstUpdate, fOldPostponed;

    {
        LOCK(cs_main);
        fFirstUpdate = mapMyNameFirstUpdate.count(vchName) != 0;
        if (fFirstUpdate)
        {
            // Postpone the firstupdate, while the dialog is open
            // If OK is pressed, fPostponed is always set to false
            // If Cancel is pressed, we restore the old fPostponed value
            fOldPostponed = mapMyNameFirstUpdate[vchName].fPostponed;
            mapMyNameFirstUpdate[vchName].fPostponed = true;
        }
    }

    ConfigureNameDialog dlg(name, value, address, fFirstUpdate, this);
    dlg.setModel(walletModel);
    if (dlg.exec() == QDialog::Accepted && fFirstUpdate)
    {
        LOCK(cs_main);
        // name_firstupdate could have been sent, while the user was editing the value
        if (mapMyNameFirstUpdate.count(vchName) != 0)
            model->updateEntry(name, dlg.getReturnData(), address, NameTableEntry::NAME_NEW, CT_UPDATED);
    }
    else if (fFirstUpdate)
    {
        LOCK(cs_main);
        // If cancel was pressed, restore the old fPostponed value
        if (mapMyNameFirstUpdate.count(vchName) != 0)
            mapMyNameFirstUpdate[vchName].fPostponed = fOldPostponed;
    }
}

void ManageNamesPage::exportClicked()
{
    // CSV is currently the only supported format
    QString filename = GUIUtil::getSaveFileName(
            this,
            tr("Export Registered Names Data"), QString(),
            tr("Comma separated file (*.csv)"));

    if (filename.isNull())
        return;

    CSVModelWriter writer(filename);

    writer.setModel(proxyModel);
    // name, column, role
    writer.addColumn("Name", NameTableModel::Name, Qt::EditRole);
    writer.addColumn("Last Move", NameTableModel::Value, Qt::EditRole);
    writer.addColumn("Address", NameTableModel::Address, Qt::EditRole);
    writer.addColumn("State", NameTableModel::State, Qt::EditRole);
    writer.addColumn("Status", NameTableModel::Status, Qt::EditRole);

    if(!writer.write())
    {
        QMessageBox::critical(this, tr("Error exporting"), tr("Could not write to file %1.").arg(filename),
                              QMessageBox::Abort, QMessageBox::Abort);
    }
}
