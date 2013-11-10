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

    explicit ConfigureNameDialog(const QString &name_, const QString &data, const QString &address_, QWidget *parent = 0);
    ~ConfigureNameDialog();

    void setModel(WalletModel *walletModel);
    const QString &getReturnData() const { return returnData; }

public slots:
    void accept();
    void reject();
    void on_copyButton_clicked();
    void on_addressBookButton_clicked();
    void on_pasteButton_clicked();
    void colorButtonToggled(bool checked);
    void on_rewardAddr_textChanged(const QString &address);

private:
    QString returnData;
    Ui::ConfigureNameDialog *ui;
    WalletModel *walletModel;
    QString name, address;

    bool reward_set;

    void LoadJSON(const std::string &jsonStr);
};

#endif // CONFIGURENAMEDIALOG_H
