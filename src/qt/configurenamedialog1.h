#ifndef CONFIGURENAMEDIALOG1_H
#define CONFIGURENAMEDIALOG1_H

#include <QDialog>

#include "../json/json_spirit.h"


namespace Ui {
    class ConfigureNameDialog1;
}

class WalletModel;

/** Dialog for editing an address and associated information.
 */
class ConfigureNameDialog1 : public QDialog
{
    Q_OBJECT

public:

    explicit ConfigureNameDialog1(const QString &name_, const std::string &data, QWidget *parent = 0);
    ~ConfigureNameDialog1();

    void setModel(WalletModel *walletModel);
    const std::string &getReturnData() const { return returnData; }

public slots:
    void accept();
    void reject();
    void on_addressBookButton_clicked();
    void on_pasteButton_clicked();
    void colorButtonToggled(bool checked);
    void on_rewardAddr_textChanged(const QString &address);

private:
    std::string returnData;
    Ui::ConfigureNameDialog1 *ui;
    WalletModel *walletModel;
    QString name;

    bool reward_set;

    void LoadJSON(const std::string &jsonStr);
};

#endif // CONFIGURENAMEDIALOG1_H
