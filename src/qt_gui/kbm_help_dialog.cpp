// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "kbm_help_dialog.h"

#include <QApplication>

#include <QGroupBox>

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

class ExpandableSection : public QWidget {
public:
    ExpandableSection(const QString& title, const QString& content, QWidget* parent = nullptr)
        : QWidget(parent) {
        QVBoxLayout* layout = new QVBoxLayout;

        // Create a button for the section title
        QPushButton* toggleButton = new QPushButton(title);
        layout->addWidget(toggleButton);

        // Create a label for the content (initially hidden) with word wrap
        QLabel* contentLabel = new QLabel(content);
        contentLabel->setWordWrap(true);
        contentLabel->setVisible(false); // Hidden by default
        layout->addWidget(contentLabel);

        // Connect button click to toggle content visibility
        connect(toggleButton, &QPushButton::clicked,
                [contentLabel]() { contentLabel->setVisible(!contentLabel->isVisible()); });

        // Set spacing to 0 for minimal spacing
        layout->setSpacing(2);                  // Minimal spacing between sections
        layout->setContentsMargins(0, 0, 0, 0); // No margins for top, left, right, bottom

        setLayout(layout);
    }
};

HelpDialog::HelpDialog(QWidget* parent) : QDialog(parent) {
    // Main layout for the help dialog
    QVBoxLayout* mainLayout = new QVBoxLayout;

    // Add expandable sections
    mainLayout->addWidget(new ExpandableSection("Quickstart", quickstart()));
    mainLayout->addWidget(new ExpandableSection("FAQ", faq()));
    mainLayout->addWidget(new ExpandableSection("Syntax", syntax()));
    mainLayout->addWidget(new ExpandableSection("Special Bindings", special()));
    mainLayout->addWidget(new ExpandableSection("Keybindings", bindings()));

    // Create a widget to hold all sections (with unified scrollable area)
    QWidget* containerWidget = new QWidget;
    containerWidget->setLayout(mainLayout);

    // Create a scroll area to wrap all content
    QScrollArea* scrollArea = new QScrollArea;
    scrollArea->setWidgetResizable(true);
    scrollArea->setWidget(containerWidget);

    mainLayout->setAlignment(Qt::AlignTop);

    // Main layout for the Help dialog
    QVBoxLayout* dialogLayout = new QVBoxLayout;
    dialogLayout->addWidget(scrollArea);
    setLayout(dialogLayout);

    setMinimumSize(500, 400); // Set a reasonable window size// Set the layout for the dialog
}
