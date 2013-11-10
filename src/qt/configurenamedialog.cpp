#include "configurenamedialog.h"
#include "ui_configurenamedialog.h"

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

ConfigureNameDialog::ConfigureNameDialog(const QString &name_, const QString &data, const QString &address_, QWidget *parent) :
    QDialog(parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint),
    name(name_),
    address(address_),
    ui(new Ui::ConfigureNameDialog)
{
    ui->setupUi(this);

#ifdef Q_OS_MAC
    ui->rewardAddrLayout->setSpacing(4);
#endif

    GUIUtil::setupAddressWidget(ui->rewardAddr, this, true);

    ui->labelName->setText(GUIUtil::HtmlEscape(name));
    ui->labelAddress->setText(GUIUtil::HtmlEscape(address));
    ui->labelAddress->setFont(GUIUtil::bitcoinAddressFont());

    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);

    returnData = data;
    if (data.isEmpty())
        reward_set = false;
    else
    {
        LoadJSON(data.toStdString());
        reward_set = true;
    }

    connect(ui->yellowButton, SIGNAL(toggled(bool)), this, SLOT(colorButtonToggled(bool)));
    connect(ui->redButton, SIGNAL(toggled(bool)), this, SLOT(colorButtonToggled(bool)));
    connect(ui->greenButton, SIGNAL(toggled(bool)), this, SLOT(colorButtonToggled(bool)));
    connect(ui->blueButton, SIGNAL(toggled(bool)), this, SLOT(colorButtonToggled(bool)));
}

void ConfigureNameDialog::LoadJSON(const std::string &jsonStr)
{
    reward_set = false;

    using namespace json_spirit;
    Value v;
    if (!json_spirit::read_string(jsonStr, v) || v.type() != obj_type)
        return;
    Object obj = v.get_obj();

    v = find_value(obj, "color");
    if (v.type() != null_type)
    {
        int color = v.get_int();
        if (color == 0)
            ui->yellowButton->setChecked(true);
        else if (color == 1)
            ui->redButton->setChecked(true);
        else if (color == 2)
            ui->greenButton->setChecked(true);
        else if (color == 3)
            ui->blueButton->setChecked(true);
        if (color >= 0 && color <= 3)
            ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
    }

    v = find_value(obj, "message");
    if (v.type() == str_type)
        ui->messageEdit->setPlainText(QString::fromStdString(v.get_str()));

    v = find_value(obj, "address");
    if (v.type() == str_type)
        ui->rewardAddr->setText(QString::fromStdString(v.get_str()));
}

ConfigureNameDialog::~ConfigureNameDialog()
{
    delete ui;
}

void ConfigureNameDialog::accept()
{
    if (!walletModel)
        return;

    if (!ui->rewardAddr->hasAcceptableInput())
    {
        ui->rewardAddr->setValid(false);
        return;
    }

    json_spirit::Object obj;
    QString reward = ui->rewardAddr->text();
    if (!reward.isEmpty())
        obj.push_back(json_spirit::Pair("address", reward.toStdString()));

    std::string msgStr = ui->messageEdit->toPlainText().toStdString();
    if (!msgStr.empty())
        obj.push_back(json_spirit::Pair("message", msgStr));

    int color;
    if (ui->yellowButton->isChecked())
        color = 0;
    else if (ui->redButton->isChecked())
        color = 1;
    else if (ui->greenButton->isChecked())
        color = 2;
    else if (ui->blueButton->isChecked())
        color = 3;
    else
    {
        QMessageBox::warning(this, tr("Player spawn"), "Please select player color");
        return;
    }
    obj.push_back(json_spirit::Pair("color", color));

    json_spirit::Value val(obj);
    std::string jsonStr = json_spirit::write_string(val, false);

    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return;

    returnData = QString::fromStdString(jsonStr);

    QString err_msg;
    try
    {
        err_msg = walletModel->nameFirstUpdatePrepare(name, returnData, false);
    }
    catch (std::exception& e) 
    {
        err_msg = e.what();
    }

    if (!err_msg.isEmpty())
    {
        if (err_msg == "ABORTED")
            return;

        QMessageBox::critical(this, tr("Name update error"), err_msg);
        return;
    }

    if (ui->useRewardAddressForNewPlayers->isChecked() && reward != walletModel->getOptionsModel()->getDefaultRewardAddress())
        walletModel->getOptionsModel()->setDefaultRewardAddress(reward);

    QDialog::accept();
}

void ConfigureNameDialog::reject()
{
    QDialog::reject();
}

void ConfigureNameDialog::setModel(WalletModel *walletModel)
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

void ConfigureNameDialog::on_copyButton_clicked()
{
    QApplication::clipboard()->setText(address);
}

void ConfigureNameDialog::on_pasteButton_clicked()
{
    // Paste text from clipboard into reward address field
    ui->rewardAddr->setText(QApplication::clipboard()->text());
}

void ConfigureNameDialog::on_addressBookButton_clicked()
{
    if (!walletModel)
        return;
    AddressBookPage dlg(AddressBookPage::ForSending, AddressBookPage::SendingTab, this);
    dlg.setModel(walletModel->getAddressTableModel());
    if (dlg.exec())
        ui->rewardAddr->setText(dlg.getReturnValue());
}

void ConfigureNameDialog::colorButtonToggled(bool checked)
{
    if (checked)
        ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(true);
}

void ConfigureNameDialog::on_rewardAddr_textChanged(const QString &address)
{
    if (ui->useRewardAddressForNewPlayers->isChecked() && address != walletModel->getOptionsModel()->getDefaultRewardAddress())
        ui->useRewardAddressForNewPlayers->setChecked(false);
}
