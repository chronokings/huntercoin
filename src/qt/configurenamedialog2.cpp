#include "configurenamedialog2.h"
#include "ui_configurenamedialog2.h"

#include "guiutil.h"
#include "addressbookpage.h"
#include "walletmodel.h"
#include "optionsmodel.h"
#include "guiconstants.h"
#include "../headers.h"
#include "../huntercoin.h"
#include "../gamestate.h"
#include "../gamedb.h"

#include "../json/json_spirit.h"
#include "../json/json_spirit_utils.h"
#include "../json/json_spirit_reader_template.h"
#include "../json/json_spirit_writer_template.h"

#include <boost/foreach.hpp>

#include <QMessageBox>
#include <QPushButton>
#include <QClipboard>

ConfigureNameDialog2::ConfigureNameDialog2(const QString &name_, const QString &address_, const QString &rewardAddr_, const QString &transferTo_, QWidget *parent)
    : QDialog(parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint),
    name(name_),
    address(address_),
    rewardAddr(rewardAddr_),
    transferTo(transferTo_),
    ui(new Ui::ConfigureNameDialog2)
{
    ui->setupUi(this);

#ifdef Q_OS_MAC
    ui->rewardAddrLayout->setSpacing(4);
    ui->transferToLayout->setSpacing(4);
#endif

    GUIUtil::setupAddressWidget(ui->rewardAddr, this, true);
    GUIUtil::setupAddressWidget(ui->transferTo, this, true);

    ui->labelName->setText(GUIUtil::HtmlEscape(name));
    ui->labelAddress->setText(GUIUtil::HtmlEscape(address));
    ui->labelAddress->setFont(GUIUtil::bitcoinAddressFont());

    ui->rewardAddr->setText(rewardAddr);
    ui->transferTo->setText(transferTo);
    reward_set = !rewardAddr.isEmpty();
}

ConfigureNameDialog2::~ConfigureNameDialog2()
{
    delete ui;
}


void ConfigureNameDialog2::accept()
{
    if (!walletModel)
        return;

    if (!ui->rewardAddr->hasAcceptableInput())
    {
        ui->rewardAddr->setValid(false);
        return;
    }

    rewardAddr = ui->rewardAddr->text();
    if (!rewardAddr.isEmpty() && !walletModel->validateAddress(rewardAddr))
    {
        ui->rewardAddr->setValid(false);
        return;
    }

    if (!ui->transferTo->hasAcceptableInput())
    {
        ui->transferTo->setValid(false);
        return;
    }

    transferTo = ui->transferTo->text();
    if (!transferTo.isEmpty() && !walletModel->validateAddress(transferTo))
    {
        ui->transferTo->setValid(false);
        return;
    }

    if (ui->useRewardAddressForNewPlayers->isChecked() && rewardAddr != walletModel->getOptionsModel()->getDefaultRewardAddress())
        walletModel->getOptionsModel()->setDefaultRewardAddress(rewardAddr);

    QDialog::accept();
}

void ConfigureNameDialog2::reject()
{
    QDialog::reject();
}

void ConfigureNameDialog2::setModel(WalletModel *walletModel)
{
    this->walletModel = walletModel;
    if (!reward_set)
    {
        ui->rewardAddr->setText(walletModel->getOptionsModel()->getDefaultRewardAddress());
        ui->useRewardAddressForNewPlayers->setChecked(true);
    }
    else
        ui->useRewardAddressForNewPlayers->setChecked(walletModel->getOptionsModel()->getDefaultRewardAddress() == ui->rewardAddr->text());
}

void ConfigureNameDialog2::on_copyButton_clicked()
{
    QApplication::clipboard()->setText(address);
}

void ConfigureNameDialog2::on_pasteButton_clicked()
{
    // Paste text from clipboard into reward address field
    ui->rewardAddr->setText(QApplication::clipboard()->text());
}

void ConfigureNameDialog2::on_addressBookButton_clicked()
{
    if (!walletModel)
        return;
    AddressBookPage dlg(AddressBookPage::ForSending, AddressBookPage::SendingTab, this);
    dlg.setModel(walletModel->getAddressTableModel());
    if (dlg.exec())
        ui->rewardAddr->setText(dlg.getReturnValue());
}

void ConfigureNameDialog2::on_rewardAddr_textChanged(const QString &address)
{
    if (ui->useRewardAddressForNewPlayers->isChecked() && address != walletModel->getOptionsModel()->getDefaultRewardAddress())
        ui->useRewardAddressForNewPlayers->setChecked(false);
}

void ConfigureNameDialog2::on_pasteButton_2_clicked()
{
    // Paste text from clipboard into reward address field
    ui->transferTo->setText(QApplication::clipboard()->text());
}

void ConfigureNameDialog2::on_addressBookButton_2_clicked()
{
    if (!walletModel)
        return;
    AddressBookPage dlg(AddressBookPage::ForSending, AddressBookPage::SendingTab, this);
    dlg.setModel(walletModel->getAddressTableModel());
    if (dlg.exec())
        ui->transferTo->setText(dlg.getReturnValue());
}
