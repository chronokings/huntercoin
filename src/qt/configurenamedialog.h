#ifndef CONFIGURENAMEDIALOG_H
#define CONFIGURENAMEDIALOG_H

#include <QDialog>

#include "../json/json_spirit.h"


namespace Ui {
    class ConfigureNameDialog;
}

class WalletModel;

/** Dialog for editing an address and associated information.
 */
class ConfigureNameDialog : public QDialog
{
    Q_OBJECT

public:

    explicit ConfigureNameDialog(const QString &name_, const QString &data, const QString &address_, bool firstUpdate_, QWidget *parent = 0);
    ~ConfigureNameDialog();

    void setModel(WalletModel *walletModel);
    const QString &getReturnData() const { return returnData; }

public slots:
    void accept();
    void reject();
    void on_addressBookButton_clicked();
    void on_copyButton_clicked();
    void on_pasteButton_clicked();
    void on_dataEdit_textChanged(const QString &text);

    void on_redButton_clicked();
    void on_blueButton_clicked();
    void on_upButton_clicked();
    void on_downButton_clicked();
    void on_leftButton_clicked();
    void on_rightButton_clicked();

    void on_comboBoxAttack_currentIndexChanged(int index);

    void on_messageEdit_textChanged();

private:
    QString returnData;
    Ui::ConfigureNameDialog *ui;
    WalletModel *walletModel;
    QString name, address;
    bool firstUpdate;

    void SetJson(json_spirit::Object &obj);
};

#endif // CONFIGURENAMEDIALOG_H
