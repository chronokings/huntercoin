#ifndef CONFIGURENAMEDIALOG2_H
#define CONFIGURENAMEDIALOG2_H

#include <QDialog>

#include "../json/json_spirit.h"


namespace Ui {
    class ConfigureNameDialog2;
}

class WalletModel;

/** Dialog for editing an address and associated information.
 */
class ConfigureNameDialog2 : public QDialog
{
    Q_OBJECT

public:

    explicit ConfigureNameDialog2(const QString &name_, const QString &address_, const QString &rewardAddr_, const QString &transferTo_, QWidget *parent = 0);
    ~ConfigureNameDialog2();

    void setModel(WalletModel *walletModel);
    const QString &getRewardAddr() const { return rewardAddr; }
    const QString &getTransferTo() const { return transferTo; }

public slots:
    void accept();
    void reject();
    void on_copyButton_clicked();
    void on_addressBookButton_clicked();
    void on_pasteButton_clicked();
    void on_rewardAddr_textChanged(const QString &address);
    void on_addressBookButton_2_clicked();
    void on_pasteButton_2_clicked();

private:
    Ui::ConfigureNameDialog2 *ui;
    WalletModel *walletModel;
    QString name, address, rewardAddr, transferTo;

    bool reward_set;
};

#endif // CONFIGURENAMEDIALOG2_H
