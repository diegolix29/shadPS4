// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <toml.hpp>

#include "common/path_util.h"

#include "../sdl_window.h"
#include "control_settings.h"
#include "ui_control_settings.h"

ControlSettings::ControlSettings(QWidget* parent)
    : QDialog(parent)

      ,
      ui(new Ui::ControlSettings) {

    ui->setupUi(this);
    AddBoxItems();
    SetUIValuestoMappings();

    connect(ui->buttonBox, &QDialogButtonBox::clicked, this, [this](QAbstractButton* button) {
        if (button == ui->buttonBox->button(QDialogButtonBox::Save)) {
            SaveControllerConfig();
            QWidget::close();
        } else if (button == ui->buttonBox->button(QDialogButtonBox::RestoreDefaults)) {
            SetDefault();
        }
    });

    connect(ui->buttonBox, &QDialogButtonBox::rejected, this, &QWidget::close);
}

void ControlSettings::SaveControllerConfig() {

    toml::ordered_value data = toml::parse<toml::ordered_type_config>("Controller.toml");

    data["A_button"]["remap"] = ui->ABox->currentText().toStdString();
    data["Y_button"]["remap"] = ui->YBox->currentText().toStdString();
    data["X_button"]["remap"] = ui->XBox->currentText().toStdString();
    data["B_button"]["remap"] = ui->BBox->currentText().toStdString();
    data["Left_bumper"]["remap"] = ui->LBBox->currentText().toStdString();
    data["Right_bumper"]["remap"] = ui->RBBox->currentText().toStdString();
    data["Left_trigger"]["remap"] = ui->LTBox->currentText().toStdString();
    data["Right_trigger"]["remap"] = ui->RTBox->currentText().toStdString();
    data["dpad_up"]["remap"] = ui->DpadUpBox->currentText().toStdString();
    data["dpad_down"]["remap"] = ui->DpadDownBox->currentText().toStdString();
    data["dpad_left"]["remap"] = ui->DpadLeftBox->currentText().toStdString();
    data["dpad_right"]["remap"] = ui->DpadRightBox->currentText().toStdString();
    data["Left_stick_button"]["remap"] = ui->LClickBox->currentText().toStdString();
    data["Right_stick_button"]["remap"] = ui->RClickBox->currentText().toStdString();
    data["Start"]["remap"] = ui->StartBox->currentText().toStdString();
    data["If_Left_analog_stick_mapped_to_buttons"]["Left_stick_up_remap"] =
        ui->LStickUpBox->currentText().toStdString();
    data["If_Left_analog_stick_mapped_to_buttons"]["Left_stick_down_remap"] =
        ui->LStickDownBox->currentText().toStdString();
    data["If_Left_analog_stick_mapped_to_buttons"]["Left_stick_left_remap"] =
        ui->LStickLeftBox->currentText().toStdString();
    data["If_Left_analog_stick_mapped_to_buttons"]["Left_stick_right_remap"] =
        ui->LStickRightBox->currentText().toStdString();
    data["If_Right_analog_stick_mapped_to_buttons"]["Right_stick_up_remap"] =
        ui->RStickUpBox->currentText().toStdString();
    data["If_Right_analog_stick_mapped_to_buttons"]["Right_stick_down_remap"] =
        ui->RStickDownBox->currentText().toStdString();
    data["If_Right_analog_stick_mapped_to_buttons"]["Right_stick_left_remap"] =
        ui->RStickLeftBox->currentText().toStdString();
    data["If_Right_analog_stick_mapped_to_buttons"]["Right_stick_right_remap"] =
        ui->RStickRightBox->currentText().toStdString();

    data["Left_analog_stick_behavior"]["Mapped_to_buttons"] = (ui->LStickButtons->isChecked());
    data["Left_analog_stick_behavior"]["Swap_sticks"] = (ui->LStickSwap->isChecked());
    data["Left_analog_stick_behavior"]["Invert_movement_vertical"] =
        (ui->LStickInvertY->isChecked());
    data["Left_analog_stick_behavior"]["Invert_movement_horizontal"] =
        (ui->LStickInvertX->isChecked());

    data["Right_analog_stick_behavior"]["Mapped_to_buttons"] = (ui->RStickButtons->isChecked());
    data["Right_analog_stick_behavior"]["Swap_sticks"] = (ui->RStickSwap->isChecked());
    data["Right_analog_stick_behavior"]["Invert_movement_vertical"] =
        (ui->RStickInvertY->isChecked());
    data["Right_analog_stick_behavior"]["Invert_movement_horizontal"] =
        (ui->RStickInvertX->isChecked());

    std::ofstream remaptoml("Controller.toml");
    remaptoml << data;
    remaptoml.close();

    Frontend::RefreshMappings();
}

void ControlSettings::SetDefault() {

    ui->ABox->setCurrentIndex(0);
    ui->BBox->setCurrentIndex(1);
    ui->XBox->setCurrentIndex(2);
    ui->YBox->setCurrentIndex(3);
    ui->DpadUpBox->setCurrentIndex(11);
    ui->DpadDownBox->setCurrentIndex(12);
    ui->DpadLeftBox->setCurrentIndex(13);
    ui->DpadRightBox->setCurrentIndex(14);
    ui->LClickBox->setCurrentIndex(8);
    ui->RClickBox->setCurrentIndex(9);
    ui->LBBox->setCurrentIndex(4);
    ui->RBBox->setCurrentIndex(5);
    ui->LTBox->setCurrentIndex(6);
    ui->RTBox->setCurrentIndex(7);
    ui->StartBox->setCurrentIndex(10);

    ui->LStickUpBox->setCurrentIndex(11);
    ui->LStickDownBox->setCurrentIndex(12);
    ui->LStickLeftBox->setCurrentIndex(13);
    ui->LStickRightBox->setCurrentIndex(14);
    ui->RStickUpBox->setCurrentIndex(3);
    ui->RStickDownBox->setCurrentIndex(0);
    ui->RStickLeftBox->setCurrentIndex(2);
    ui->RStickRightBox->setCurrentIndex(1);

    ui->LStickButtons->setChecked(false);
    ui->LStickInvertX->setChecked(false);
    ui->LStickInvertY->setChecked(false);
    ui->LStickSwap->setChecked(false);
    ui->RStickButtons->setChecked(false);
    ui->RStickInvertX->setChecked(false);
    ui->RStickInvertY->setChecked(false);
    ui->RStickSwap->setChecked(false);
}

void ControlSettings::AddBoxItems() {

    QStringList Inputs = {"cross",       "circle",     "square",     "triangle",   "L1",
                          "R1",          "L2",         "R2",         "L3",

                          "R3",          "options",    "dpad_up",

                          "dpad_down",

                          "dpad_left",   "dpad_right", "lstickup",   "lstickdown", "lstickleft",
                          "lstickright", "rstickup",   "rstickdown", "rstickleft", "rstickright"};

    QStringList InputsButtons =

        {"cross",     "circle",    "square",    "triangle", "L1", "R1",

         "L2",

         "R2",

         "L3",        "R3",        "options",   "dpad_up",

         "dpad_down", "dpad_left", "dpad_right"};

    ui->DpadUpBox->addItems(Inputs);
    ui->DpadDownBox->addItems(Inputs);
    ui->DpadLeftBox->addItems(Inputs);
    ui->DpadRightBox->addItems(Inputs);
    ui->LBBox->addItems(Inputs);
    ui->RBBox->addItems(Inputs);
    ui->LTBox->addItems(Inputs);
    ui->RTBox->addItems(Inputs);
    ui->RClickBox->addItems(Inputs);
    ui->LClickBox->addItems(Inputs);
    ui->StartBox->addItems(Inputs);
    ui->ABox->addItems(Inputs);
    ui->BBox->addItems(Inputs);
    ui->XBox->addItems(Inputs);
    ui->YBox->addItems(Inputs);

    ui->LStickUpBox->addItems(InputsButtons);
    ui->LStickDownBox->addItems(InputsButtons);
    ui->LStickLeftBox->addItems(InputsButtons);
    ui->LStickRightBox->addItems(InputsButtons);
    ui->RStickUpBox->addItems(InputsButtons);
    ui->RStickDownBox->addItems(InputsButtons);
    ui->RStickLeftBox->addItems(InputsButtons);
    ui->RStickRightBox->addItems(InputsButtons);
}

void ControlSettings::SetUIValuestoMappings() {

    toml::value data = toml::parse("Controller.toml");

    ui->LStickButtons->setChecked(
        toml::find_or<bool>(data, "Left_analog_stick_behavior", "Mapped_to_buttons", false));
    ui->LStickInvertX->setChecked(toml::find_or<bool>(data, "Left_analog_stick_behavior",
                                                      "Invert_movement_horizontal", false));
    ui->LStickInvertY->setChecked(
        toml::find_or<bool>(data, "Left_analog_stick_behavior", "Invert_movement_vertical", false));
    ui->LStickSwap->setChecked(
        toml::find_or<bool>(data, "Left_analog_stick_behavior", "Swap_sticks", false));
    ui->RStickButtons->setChecked(
        toml::find_or<bool>(data, "Right_analog_stick_behavior", "Mapped_to_buttons", false));
    ui->RStickInvertX->setChecked(toml::find_or<bool>(data, "Right_analog_stick_behavior",
                                                      "Invert_movement_horizontal", false));
    ui->RStickInvertY->setChecked(toml::find_or<bool>(data, "Right_analog_stick_behavior",
                                                      "Invert_movement_vertical", false));
    ui->RStickSwap->setChecked(
        toml::find_or<bool>(data, "Right_analog_stick_behavior", "Swap_sticks", false));

    ui->ABox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "A_button", "remap", "cross")));
    ui->BBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "B_button", "remap", "circle")));
    ui->XBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "X_button", "remap", "square")));
    ui->YBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "Y_button", "remap", "triangle")));
    ui->DpadUpBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "dpad_up", "remap", "dpad_up")));
    ui->DpadDownBox->setCurrentText(QString::fromStdString(
        toml::find_or<std::string>(data, "dpad_down", "remap", "dpad_down")));
    ui->DpadLeftBox->setCurrentText(QString::fromStdString(
        toml::find_or<std::string>(data, "dpad_left", "remap", "dpad_left")));
    ui->DpadRightBox->setCurrentText(QString::fromStdString(
        toml::find_or<std::string>(data, "dpad_right", "remap", "dpad_right")));
    ui->LClickBox->setCurrentText(QString::fromStdString(
        toml::find_or<std::string>(data, "Left_stick_button", "remap", "L3")));
    ui->RClickBox->setCurrentText(QString::fromStdString(
        toml::find_or<std::string>(data, "Right_stick_button", "remap", "R3")));
    ui->LBBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "Left_bumper", "remap", "L1")));
    ui->RBBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "Right_bumper", "remap", "R1")));
    ui->LTBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "Left_trigger", "remap", "L2")));
    ui->RTBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "Right_trigger", "remap", "R2")));
    ui->LStickUpBox->setCurrentText(QString::fromStdString(toml::find_or<std::string>(
        data, "If_Left_analog_stick_mapped_to_buttons", "Left_stick_up_remap", "lstickup")));
    ui->LStickDownBox->setCurrentText(QString::fromStdString(toml::find_or<std::string>(
        data, "If_Left_analog_stick_mapped_to_buttons", "Left_stick_down_remap", "lstickdown")));
    ui->LStickLeftBox->setCurrentText(QString::fromStdString(toml::find_or<std::string>(
        data, "If_Left_analog_stick_mapped_to_buttons", "Left_stick_left_remap", "lstickleft")));
    ui->LStickRightBox->setCurrentText(QString::fromStdString(toml::find_or<std::string>(
        data, "If_Left_analog_stick_mapped_to_buttons", "Left_stick_right_remap", "lstickright")));
    ui->RStickUpBox->setCurrentText(QString::fromStdString(toml::find_or<std::string>(
        data, "If_Right_analog_stick_mapped_to_buttons", "Right_stick_up_remap", "rstickup")));
    ui->RStickDownBox->setCurrentText(QString::fromStdString(toml::find_or<std::string>(
        data, "If_Right_analog_stick_mapped_to_buttons", "Right_stick_down_remap", "rstickdown")));
    ui->RStickLeftBox->setCurrentText(QString::fromStdString(toml::find_or<std::string>(
        data, "If_Right_analog_stick_mapped_to_buttons", "Right_stick_left_remap", "rstickleft")));
    ui->RStickRightBox->setCurrentText(QString::fromStdString(
        toml::find_or<std::string>(data, "If_Right_analog_stick_mapped_to_buttons",
                                   "Right_stick_right_remap", "rstickright")));
    ui->StartBox->setCurrentText(
        QString::fromStdString(toml::find_or<std::string>(data, "Start", "remap", "options")));
}

ControlSettings::~ControlSettings() {} // empty desctructor