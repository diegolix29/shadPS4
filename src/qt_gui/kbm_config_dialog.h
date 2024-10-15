// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef EDITOR_DIALOG_H
#define EDITOR_DIALOG_H

#include <QDialog>
#include <QPlainTextEdit>

class EditorDialog : public QDialog {
    Q_OBJECT // Necessary for using Qt's meta-object system (signals/slots)
        public : explicit EditorDialog(QWidget* parent = nullptr); // Constructor

protected:
    void closeEvent(QCloseEvent* event) override; // Override close event

private:
    QPlainTextEdit* editor; // Editor widget for the config file
    QFont editorFont;       // To handle the text size
    QString originalConfig; // starting config string

    void loadFile(); // Function to load the config file
    void saveFile(); // Function to save the config file
    bool hasUnsavedChanges();

private slots:
    void onSaveClicked();   // Save button slot
    void onCancelClicked(); // Slot for handling cancel button
    void onHelpClicked();   // Slot for handling help button
    // void onTextChanged();  // Slot to detect text changes
};

#endif // EDITOR_DIALOG_H