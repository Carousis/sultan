/*
 * cashierwidget.cpp
 * Copyright 2017 - ~, Apin <apin.klas@gmail.com>
 *
 * This file is part of Sultan.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */
#include "cashierwidget.h"
#include "ui_cashierwidget.h"
#include "global_constant.h"
#include "message.h"
#include "db_constant.h"
#include "cashiertablemodel.h"
#include "cashieritem.h"
#include "guiutil.h"
#include "keyevent.h"
#include "paycashdialog.h"
#include "preference.h"
#include "global_setting_const.h"
#include "usersession.h"
#include "printer.h"
#include "escp.h"
#include "paymentcashsuccessdialog.h"
#include "searchitemdialog.h"
#include "transactionlistdialog.h"
#include "usersession.h"
#include "dbutil.h"
#include "saveloadslotdialog.h"
#include "cashierhelpdialog.h"
#include <QMessageBox>
#include <QInputDialog>
#include <QKeyEvent>
#include <QShortcut>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <functional>

using namespace LibG;
using namespace LibGUI;
using namespace LibPrint;

CashierWidget::CashierWidget(LibG::MessageBus *bus, QWidget *parent) :
    QWidget(parent),
    ui(new Ui::CashierWidget),
    mModel(new CashierTableModel(this)),
    mPayCashDialog(new PayCashDialog(this))
{
    ui->setupUi(this);
    setMessageBus(bus);
    ui->verticalLayout->setAlignment(Qt::AlignTop);
    ui->tableView->setModel(mModel);
    ui->tableView->verticalHeader()->hide();
    ui->tableView->setSelectionMode(QAbstractItemView::SingleSelection);
    ui->tableView->setSelectionBehavior(QAbstractItemView::SelectRows);
    ui->tableView->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    ui->tableView->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    GuiUtil::setColumnWidth(ui->tableView, QList<int>() << 50 << 160 << 150 << 75 << 120 << 120);
    ui->tableView->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    ui->labelVersion->setText(CONSTANT::ABOUT_APP_NAME.arg(qApp->applicationVersion()));
    auto keyevent = new KeyEvent(ui->tableView);
    keyevent->addConsumeKey(Qt::Key_Return);
    keyevent->addConsumeKey(Qt::Key_Delete);
    ui->tableView->installEventFilter(keyevent);
    connect(ui->lineBarcode, SIGNAL(returnPressed()), SLOT(barcodeEntered()));
    connect(mModel, SIGNAL(totalChanged(double)), SLOT(totalChanged(double)));
    connect(mModel, SIGNAL(selectRow(QModelIndex)), SLOT(selectRow(QModelIndex)));
    connect(keyevent, SIGNAL(keyPressed(QObject*,QKeyEvent*)), SLOT(tableKeyPressed(QObject*,QKeyEvent*)));
    connect(mPayCashDialog, SIGNAL(requestPay(double)), SLOT(payCashRequested(double)));
    new QShortcut(QKeySequence(Qt::Key_F4), this, SLOT(payCash()));
    new QShortcut(QKeySequence(Qt::Key_F5), this, SLOT(openDrawer()));
    new QShortcut(QKeySequence(Qt::Key_F2), this, SLOT(openSearch()));
    new QShortcut(QKeySequence(Qt::Key_F6), this, SLOT(openPreviousTransaction()));
    new QShortcut(QKeySequence(Qt::Key_PageDown), this, SLOT(updateLastInputed()));
    new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_Delete), this, SLOT(newTransaction()));
    new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_N), this, SLOT(newTransaction()));
    new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_S), this, SLOT(saveCartTriggered()));
    new QShortcut(QKeySequence(Qt::CTRL + Qt::Key_O), this, SLOT(loadCartTriggered()));
    new QShortcut(QKeySequence(Qt::Key_F1), this, SLOT(openHelp()));
    ui->labelTitle->setText(Preference::getString(SETTING::MARKET_NAME, "Sultan Minimarket"));
    ui->labelSubtitle->setText(GuiUtil::toHtml(Preference::getString(SETTING::MARKET_SUBNAME, "Jln. Bantul\nYogyakarta")));
}

CashierWidget::~CashierWidget()
{
    delete ui;
}

void CashierWidget::showEvent(QShowEvent *event)
{
    ui->lineBarcode->setFocus(Qt::TabFocusReason);
    QWidget::showEvent(event);
}

void CashierWidget::messageReceived(LibG::Message *msg)
{
    if(msg->isTypeCommand(MSG_TYPE::ITEM, MSG_COMMAND::CASHIER_PRICE)) {
        ui->lineBarcode->selectAll();
        ui->lineBarcode->setEnabled(true);
        if(msg->isSuccess()) {
            const QString &name = msg->data("item").toMap()["name"].toString();
            const QString &barcode = msg->data("item").toMap()["barcode"].toString();
            ui->labelName->setText(name);
            const QVariantList &list = msg->data("prices").toList();
            double price = list.first().toMap()["price"].toDouble();
            for(int i = 1; i < list.size(); i++) {
                if(list[i].toMap()["count"].toFloat() == 1.0f) {
                    price = list[i].toMap()["price"].toDouble();
                    break;
                }
            }
            ui->labelPrice->setText(Preference::toString(price));
            mModel->addItem(mCount, name, barcode, list);
        } else {
            QMessageBox::critical(this, tr("Error"), msg->data("error").toString());
        }
    } else if(msg->isTypeCommand(MSG_TYPE::SOLD, MSG_COMMAND::NEW_SOLD)) {
        if(msg->isSuccess()) {
            const QVariantMap &data = msg->data();
            mPayCashDialog->hide();
            openDrawer();
            printBill(data);
            PaymentCashSuccessDialog dialog(data["total"].toDouble(), data["payment"].toDouble(),  data["payment"].toDouble() - data["total"].toDouble());
            dialog.exec();
            mModel->reset();
            if(mSaveSlot >= 0) removeSlot(mSaveSlot);
        } else {
            QMessageBox::critical(this, tr("Error"), msg->data("error").toString());
        }
    }
}

void CashierWidget::cutPaper()
{
    const QString &command = Escp::cutPaperCommand();
    int type = Preference::getInt(SETTING::PRINTER_CASHIER_TYPE);
    Printer::instance()->print(type == PRINT_TYPE::DEVICE ? Preference::getString(SETTING::PRINTER_CASHIER_DEVICE) : Preference::getString(SETTING::PRINTER_CASHIER_NAME),
                               command, type);
}

void CashierWidget::saveToSlot(int slot)
{
    QFile file(QString("trans_%1.trans").arg(slot));
    if(file.exists()) file.remove();
    if(!file.open(QFile::WriteOnly)) {
        QMessageBox::critical(this, tr("Error"), tr("Unable to open save file"));
        return;
    }
    const QJsonDocument &doc = QJsonDocument::fromVariant(mModel->getCart());
    file.write(qCompress(doc.toJson(QJsonDocument::Compact)));
    file.close();
}

void CashierWidget::loadFromSlot(int slot)
{
    QFile file(QString("trans_%1.trans").arg(slot));
    if(!file.exists()) {
        QMessageBox::critical(this, tr("Error"), tr("File not exists"));
        return;
    }
    if(!file.open(QFile::ReadOnly)) {
        QMessageBox::critical(this, tr("Error"), tr("Unable to open file"));
        return;
    }
    QJsonParseError err;
    const QJsonDocument &doc = QJsonDocument::fromJson(qUncompress(file.readAll()), &err);
    if(err.error != QJsonParseError::NoError) {
        QMessageBox::critical(this, tr("Error"), tr("Error parsing json file"));
        return;
    }
    mModel->loadCart(doc.array().toVariantList());
    ui->tableView->selectRow(mModel->rowCount(QModelIndex()) - 1);
    file.close();
}

void CashierWidget::removeSlot(int slot)
{
    QFile file(QString("trans_%1.trans").arg(slot));
    if(file.exists()) file.remove();
    mSaveSlot = -1;
}

void CashierWidget::barcodeEntered()
{
    QString barcode = ui->lineBarcode->text();
    if(barcode.isEmpty()) return;
    if(barcode.contains("*")) {
        const QStringList ls = barcode.split("*");
        if(ls.size() > 1) {
            barcode = ls[1];
            mCount = ls[0].toFloat();
        }
    } else {
        mCount = 1.0f;
    }
    LibG::Message msg(MSG_TYPE::ITEM, MSG_COMMAND::CASHIER_PRICE);
    msg.addData("barcode", barcode);
    sendMessage(&msg);
}

void CashierWidget::totalChanged(double value)
{
    ui->labelTotal->setText(Preference::toString(value));
}

void CashierWidget::selectRow(const QModelIndex &index)
{
    ui->tableView->selectRow(index.row());
    ui->tableView->scrollTo(index);
}

void CashierWidget::tableKeyPressed(QObject */*sender*/, QKeyEvent *event)
{
    const QModelIndex &index = ui->tableView->currentIndex();
    if(!index.isValid()) return;
    auto item = static_cast<CashierItem*>(index.internalPointer());
    if(event->key() == Qt::Key_Return) {
        bool ok = false;
        double count = QInputDialog::getDouble(this, tr("Edit count"), tr("Input new count"), item->count, 0, 1000000, 1, &ok);
        if(ok)
            mModel->addItem(count - item->count, item->name, item->barcode);
    } else if(event->key() == Qt::Key_Delete){
        mModel->addItem(-item->count, item->name, item->barcode);
    }
}

void CashierWidget::payCash()
{
    if(mModel->isEmpty()) return;
    mPayCashDialog->fill(mModel->getTotal());
    mPayCashDialog->show();
}

void CashierWidget::payCashless()
{
}

void CashierWidget::openDrawer()
{
    const QString &command = Escp::openDrawerCommand();
    int type = Preference::getInt(SETTING::PRINTER_CASHIER_TYPE);
    Printer::instance()->print(type == PRINT_TYPE::DEVICE ? Preference::getString(SETTING::PRINTER_CASHIER_DEVICE) : Preference::getString(SETTING::PRINTER_CASHIER_NAME),
                               command, type);
}

void CashierWidget::updateLastInputed()
{
    const QModelIndex &index = ui->tableView->currentIndex();
    if(!index.isValid()) return;
    auto item = static_cast<CashierItem*>(index.internalPointer());
    bool ok = false;
    double count = QInputDialog::getDouble(this, tr("Edit count"), tr("Input new count"), item->count, 0, 1000000, 1, &ok);
    if(ok)
        mModel->addItem(count - item->count, item->name, item->barcode);
}

void CashierWidget::payCashRequested(double value)
{
    QVariantMap data;
    data.insert("cart", mModel->getCart());
    data.insert("user_id", UserSession::id());
    data.insert("machine_id", 1);
    data.insert("total", mModel->getTotal());
    data.insert("payment", value);
    Message msg(MSG_TYPE::SOLD, MSG_COMMAND::NEW_SOLD);
    msg.setData(data);
    sendMessage(&msg);
}

void CashierWidget::printBill(const QVariantMap &data)
{
    int type = Preference::getInt(SETTING::PRINTER_CASHIER_TYPE, -1);
    if(type < 0) {
        QMessageBox::critical(this, tr("Error"), tr("Please setting printer first"));
        return;
    }
    const QString &prName = Preference::getString(SETTING::PRINTER_CASHIER_NAME);
    const QString &prDevice = Preference::getString(SETTING::PRINTER_CASHIER_DEVICE);
    const QString &title = Preference::getString(SETTING::PRINTER_CASHIER_TITLE, "Sultan Minimarket");
    const QString &subtitle = Preference::getString(SETTING::PRINTER_CASHIER_SUBTITLE, "Jogonalan Lor RT 2 Bantul");
    const QString &footer = Preference::getString(SETTING::PRINTER_CASHIER_FOOTER, "Barang dibeli tidak dapat ditukar");
    int cpi10 = Preference::getInt(SETTING::PRINTER_CASHIER_CPI10, 32);
    int cpi12 = Preference::getInt(SETTING::PRINTER_CASHIER_CPI12, 40);

    auto escp = new Escp(Escp::SIMPLE, cpi10, cpi12);
    escp->cpi10()->doubleHeight(true)->centerText(title)->newLine()->doubleHeight(false)->cpi12()->
            centerText(subtitle)->newLine(2);
    escp->leftText(LibDB::DBUtil::sqlDateToDateTime(data["created_at"].toString()).toString("dd-MM-yy hh:mm"))->newLine();
    escp->column(QList<int>{50, 50})->leftText(data["number"].toString())->rightText(UserSession::username());
    escp->newLine()->column(QList<int>())->line(QChar('='));
    const QVariantList &l = data["cart"].toList();
    for(auto v : l) {
        QVariantMap m = v.toMap();
        escp->leftText(m["name"].toString())->newLine();
        escp->column(QList<int>{50, 50})->leftText(QString("%1 x %2").
                                                   arg(Preference::toString(m["count"].toFloat())).
                                                    arg(Preference::toString(m["price"].toDouble())));
        escp->rightText(Preference::toString(m["total"].toDouble()))->column(QList<int>())->newLine();
    }
    escp->line();
    escp->column(QList<int>{50, 50})->leftText(tr("Total"))->rightText(Preference::toString(data["total"].toDouble()))->newLine()->
            leftText(tr("Payment"))->rightText(Preference::toString(data["payment"].toDouble()))->newLine()->
            leftText(tr("Change"))->rightText(Preference::toString(data["payment"].toDouble() - data["total"].toDouble()))->newLine();
    escp->column(QList<int>())->doubleHeight(false)->line()->newLine()->leftText(footer, true)->newLine(3);
    Printer::instance()->print(type == PRINT_TYPE::DEVICE ? prDevice : prName, escp->data(), type);
    delete escp;
    cutPaper();
}

void CashierWidget::openSearch()
{
    SearchItemDialog dialog(mMessageBus);
    dialog.exec();
    const QString &barcode = dialog.getSelectedBarcode();
    if(barcode.isEmpty()) return;
    ui->lineBarcode->setText(barcode);
    barcodeEntered();
}

void CashierWidget::openPreviousTransaction()
{
    TransactionListDialog dialog(mMessageBus);
    dialog.setPrintFunction(std::bind(&CashierWidget::printBill, this, std::placeholders::_1));
    dialog.exec();
}

void CashierWidget::newTransaction()
{
    if(mModel->isEmpty()) return;
    if(mSaveSlot >= 0) {
        saveToSlot(mSaveSlot);
    } else {
        int res = QMessageBox::question(this, tr("New transaction confirmation"), tr("Are you sure want to ignore this transaction and start new one?"));
        if(res != QMessageBox::Yes) return;
    }
    mModel->reset();
    mSaveSlot = -1;
}

void CashierWidget::saveCartTriggered()
{
    if(mModel->isEmpty()) return;
    if(mSaveSlot < 0) {
        SaveLoadSlotDialog dialog(this);
        dialog.exec();
        if(dialog.getSelectedSlot() < 0) return;
        saveToSlot(dialog.getSelectedSlot());
        mSaveSlot = dialog.getSelectedSlot();
    }
}

void CashierWidget::loadCartTriggered()
{
    if(!mModel->isEmpty() && mSaveSlot < 0) {
        int ret = QMessageBox::question(this, tr("Confirmation"), tr("Your cart is not empty, do you want to ignore current cart?"));
        if(ret != QMessageBox::Yes) {
            return;
        }
    } else if(!mModel->isEmpty() && mSaveSlot >= 0) {
        saveToSlot(mSaveSlot);
    }
    SaveLoadSlotDialog dialog(false, this);
    dialog.exec();
    if(dialog.getSelectedSlot() < 0) return;
    loadFromSlot(dialog.getSelectedSlot());
    mSaveSlot = dialog.getSelectedSlot();
}

void CashierWidget::openHelp()
{
    CashierHelpDialog dialog(this);
    dialog.exec();
}
