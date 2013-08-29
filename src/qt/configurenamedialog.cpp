#include "configurenamedialog.h"
#include "ui_configurenamedialog.h"

#include "guiutil.h"
#include "addressbookpage.h"
#include "walletmodel.h"
#include "guiconstants.h"
#include "../headers.h"
#include "../chronokings.h"
#include "../gamestate.h"
#include "../gamedb.h"

#include "../json/json_spirit.h"
#include "../json/json_spirit_utils.h"
#include "../json/json_spirit_reader_template.h"
#include "../json/json_spirit_writer_template.h"

#include <boost/foreach.hpp>

#include <QMessageBox>
#include <QClipboard>

ConfigureNameDialog::ConfigureNameDialog(const QString &name_, const QString &data, const QString &address_, bool firstUpdate_, QWidget *parent) :
    QDialog(parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint),
    name(name_),
    address(address_),
    firstUpdate(firstUpdate_),
    ui(new Ui::ConfigureNameDialog)
{
    ui->setupUi(this);

#ifdef Q_OS_MAC
    ui->transferToLayout->setSpacing(4);
#endif

    GUIUtil::setupAddressWidget(ui->transferTo, this, true);

    ui->labelName->setText(GUIUtil::HtmlEscape(name));
    ui->dataEdit->setText(data);
    ui->labelAddress->setText(GUIUtil::HtmlEscape(address));
    ui->labelAddress->setFont(GUIUtil::bitcoinAddressFont());

    ui->dataEdit->setMaxLength(GUI_MAX_VALUE_LENGTH);

    returnData = data;

    on_dataEdit_textChanged(data);

    if (firstUpdate)
    {
        ui->labelTransferTo->hide();
        ui->labelTransferToHint->hide();
        ui->transferTo->hide();
        ui->addressBookButton->hide();
        ui->pasteButton->hide();
        ui->labelSubmitHint->setText(
            tr("Name_firstupdate transaction will be queued and broadcasted when corresponding name_new is %1 blocks old").arg(MIN_FIRSTUPDATE_DEPTH)
            + "<br/><span style='color:red'>" + tr("Do not close your client while the name is pending!") + "</span>"
        );

        ui->labelStep->setEnabled(false);
        ui->labelAttack->setEnabled(false);
        ui->upButton->setEnabled(false);
        ui->downButton->setEnabled(false);
        ui->leftButton->setEnabled(false);
        ui->rightButton->setEnabled(false);
        ui->labelReachablePlayers->setEnabled(false);
        ui->comboBoxAttack->setEnabled(false);        
    }
    else
    {
        ui->labelSpawn->setEnabled(false);
        ui->labelColor->setEnabled(false);
        ui->redButton->setEnabled(false);
        ui->blueButton->setEnabled(false);

        ui->labelSubmitHint->setText(tr("Name_update transaction will be issued immediately"));

        std::vector<std::string> victims;
        CRITICAL_BLOCK(cs_main)
            victims = GetCurrentGameState().ListPossibleAttacks(name.toStdString());
        BOOST_FOREACH(std::string victim, victims)
        {
            QString s = QString::fromStdString(victim);
            ui->comboBoxAttack->addItem(GUIUtil::HtmlEscape(s), s);
        }

        setWindowTitle(tr("Update Name"));
    }
}

ConfigureNameDialog::~ConfigureNameDialog()
{
    delete ui;
}

void ConfigureNameDialog::accept()
{
    if (!walletModel)
        return;

    QString addr;
    if (!firstUpdate)
    {
        if (!ui->transferTo->hasAcceptableInput())
        {
            ui->transferTo->setValid(false);
            return;
        }

        addr = ui->transferTo->text();

        if (addr != "" && !walletModel->validateAddress(addr))
        {
            ui->transferTo->setValid(false);
            return;
        }
    }
    
    WalletModel::UnlockContext ctx(walletModel->requestUnlock());
    if (!ctx.isValid())
        return;

    returnData = ui->dataEdit->text();

    QString err_msg;
    try
    {
        if (firstUpdate)
            err_msg = walletModel->nameFirstUpdatePrepare(name, returnData, false);
        else
            err_msg = walletModel->nameUpdate(name, returnData, addr);
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

    QDialog::accept();
}

void ConfigureNameDialog::reject()
{
    QDialog::reject();
}

void ConfigureNameDialog::setModel(WalletModel *walletModel)
{
    this->walletModel = walletModel;
}

void ConfigureNameDialog::on_copyButton_clicked()
{
    QApplication::clipboard()->setText(address);
}

void ConfigureNameDialog::on_pasteButton_clicked()
{
    // Paste text from clipboard into recipient field
    ui->transferTo->setText(QApplication::clipboard()->text());
}

void ConfigureNameDialog::on_addressBookButton_clicked()
{
    if (!walletModel)
        return;
    AddressBookPage dlg(AddressBookPage::ForSending, AddressBookPage::SendingTab, this);
    dlg.setModel(walletModel->getAddressTableModel());
    if (dlg.exec())
        ui->transferTo->setText(dlg.getReturnValue());
}

void ConfigureNameDialog::SetJson(json_spirit::Object &obj)
{
    std::string msgStr = ui->messageEdit->toPlainText().toStdString();
    if (!msgStr.empty())
        obj.push_back(json_spirit::Pair("message", msgStr));
    json_spirit::Value val(obj);
    std::string jsonStr = json_spirit::write_string(val, false);
    ui->dataEdit->setText(QString::fromStdString(jsonStr)); 
}

void ConfigureNameDialog::on_redButton_clicked()
{
    json_spirit::Object obj;
    obj.push_back(json_spirit::Pair("color", 0));
    SetJson(obj);
}

void ConfigureNameDialog::on_blueButton_clicked()
{
    json_spirit::Object obj;
    obj.push_back(json_spirit::Pair("color", 1));
    SetJson(obj);
}

void ConfigureNameDialog::on_upButton_clicked()
{
    json_spirit::Object obj;
    obj.push_back(json_spirit::Pair("deltaX", 0));
    obj.push_back(json_spirit::Pair("deltaY", -1));
    SetJson(obj);
}

void ConfigureNameDialog::on_downButton_clicked()
{
    json_spirit::Object obj;
    obj.push_back(json_spirit::Pair("deltaX", 0));
    obj.push_back(json_spirit::Pair("deltaY", 1));
    SetJson(obj);
}

void ConfigureNameDialog::on_leftButton_clicked()
{
    json_spirit::Object obj;
    obj.push_back(json_spirit::Pair("deltaX", -1));
    obj.push_back(json_spirit::Pair("deltaY", 0));
    SetJson(obj);
}

void ConfigureNameDialog::on_rightButton_clicked()
{
    json_spirit::Object obj;
    obj.push_back(json_spirit::Pair("deltaX", 1));
    obj.push_back(json_spirit::Pair("deltaY", 0));
    SetJson(obj);
}

void ConfigureNameDialog::on_comboBoxAttack_currentIndexChanged(int index)
{
    json_spirit::Object obj;
    obj.push_back(json_spirit::Pair("attack", ui->comboBoxAttack->itemData(index).toString().toStdString()));
    SetJson(obj);
}

void ConfigureNameDialog::on_messageEdit_textChanged()
{
    std::string jsonStr = ui->dataEdit->text().trimmed().toStdString();
    if (jsonStr == "")
        jsonStr = "{}";
    json_spirit::Value val;
    if (!json_spirit::read_string(jsonStr, val) || val.type() != json_spirit::obj_type)
        return;
    json_spirit::Object &obj = val.get_obj();

    // Add or replace the message field
    bool found = false;
    for (std::vector<json_spirit::Pair>::iterator it = obj.begin(); it != obj.end(); ++it)
    {
        if (it->name_ == "message")
        {
            it->value_ = ui->messageEdit->toPlainText().toStdString();
            found = true;
            break;
        }
    }
    if (!found)
        obj.push_back(json_spirit::Pair("message", ui->messageEdit->toPlainText().toStdString()));
    jsonStr = json_spirit::write_string(val, false);
    ui->dataEdit->setText(QString::fromStdString(jsonStr)); 
}

void ConfigureNameDialog::on_dataEdit_textChanged(const QString &text)
{
    Game::Move *m = Game::Move::Parse(name.toStdString(), text.toStdString());
    if (m == NULL)
    {
        ui->labelJsonValid->setText(tr("Invalid move"));
        ui->labelJsonValid->setStyleSheet("color:brown");
    }
    else
    {
        bool fValidInGameState;
        CRITICAL_BLOCK(cs_main)
            fValidInGameState = m->IsValid(GetCurrentGameState());

        if (!fValidInGameState)
        {
            ui->labelJsonValid->setText(tr("Invalid move for the current game state"));
            ui->labelJsonValid->setStyleSheet("color:brown");
        }
        else
        {
            ui->labelJsonValid->setText(tr("Valid move"));
            ui->labelJsonValid->setStyleSheet("color:green");
        }
        delete m;
    }
}
