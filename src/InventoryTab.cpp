#include "InventoryTab.h"
#include "Savegame.h"
#include <QListWidget>
#include <QTreeWidget>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QPushButton>
#include <QDebug>
#include <QLabel>
#include <QMessageBox>
#include <QSpinBox>

InventoryTab::InventoryTab(Savegame *savegame, QWidget *parent) : QWidget(parent),
  m_savegame(savegame)
{
    QVBoxLayout *tabLayout = new QVBoxLayout(this);
    QHBoxLayout *mainLayout = new QHBoxLayout;
    tabLayout->addLayout(mainLayout);

    m_list = new QListWidget;
    mainLayout->addWidget(m_list);

    m_partsList = new QTreeWidget;
    m_partsList->setHeaderHidden(true);
    mainLayout->addWidget(m_partsList);

    m_partName = new QLabel;
    m_partName->setWordWrap(true);
    m_partEffects = new QLabel;
    m_partEffects->setWordWrap(true);
    m_partNegatives = new QLabel;
    m_partNegatives->setWordWrap(true);
    m_partPositives = new QLabel;
    m_partPositives->setWordWrap(true);

    QWidget *infoWidget = new QWidget;
    QVBoxLayout *partInfoLayout = new QVBoxLayout(infoWidget);
    m_warningText = new QLabel;
    m_warningText->setWordWrap(true);
    m_warningText->setStyleSheet("background-color:yellow;");
    tabLayout->addWidget(m_warningText);

    partInfoLayout->addWidget(new QLabel(tr("<h3>Item part details</h3>")));
    partInfoLayout->addWidget(m_partName);
    partInfoLayout->addWidget(new QLabel(tr("<b>Description</b>")));
    partInfoLayout->addWidget(m_partEffects);
    partInfoLayout->addWidget(new QLabel(tr("<b>Negatives</b>")));
    partInfoLayout->addWidget(m_partNegatives);
    partInfoLayout->addWidget(new QLabel(tr("<b>Positives</b>")));
    partInfoLayout->addWidget(m_partPositives);
    partInfoLayout->addStretch();
    m_itemLevel = new QSpinBox;
    m_itemLevel->setMinimum(Constants::minLevel);
    m_itemLevel->setMaximum(Constants::maxLevel);
    partInfoLayout->addWidget(m_itemLevel);

//    infoWidget->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Expanding);
    infoWidget->setFixedWidth(300);

    QLabel *docsLink = new QLabel("<a href=https://docs.google.com/spreadsheets/d/16b7bGPFKIrNg_cJm_WCMO6cKahexBs7BiJ6ja0RlD04/edit>Data source</a>");
    docsLink->setOpenExternalLinks(true);
    partInfoLayout->addWidget(docsLink);

    mainLayout->addWidget(infoWidget);

    connect(savegame, &Savegame::itemsChanged, this, &InventoryTab::load);
    connect(m_list, &QListWidget::itemSelectionChanged, this, &InventoryTab::onItemSelected);
    connect(m_partsList, &QTreeWidget::itemSelectionChanged, this, &InventoryTab::onPartSelected);
    connect(m_partsList, &QTreeWidget::itemChanged, this, &InventoryTab::onPartChanged);
    connect(m_itemLevel, &QSpinBox::textChanged, this, &InventoryTab::onItemLevelChanged);
}

static QString makeNamePretty(const QString &name)
{
    QString displayName = name.split('.').last();
    displayName.replace("_AR_", "_Assault Rifle_");
    displayName.replace("_SR_", "_Sniper Rifle_");
    displayName.replace("_SM_", "_SMG_");
    displayName.replace("_SG_", "_Shotgun_");
    displayName.replace("_GM_", "_Grenade Mod_");
    displayName.replace("_MAL_", "_Maliwan_");
    displayName.replace("_DAL_", "_Dahl_");
    displayName.replace("_Hyp_", "_Hyperion_");
    displayName.replace("_HYP_", "_Hyperion_");
    displayName.replace("_TED_", "_Tediore_");
    displayName.replace("_VLA_", "_Vladof_");
    QStringList nameParts = displayName.split('_');
    if (nameParts.count() >= 3) {
        //            displayName = nameParts.mid(1).join(' ');
        if (nameParts.first() == "Part") {
            nameParts.takeFirst();
        }
        nameParts.replaceInStrings("SR", "Sniper Rifle");
    } else {
        qWarning() << "Weird name" << name;
    }
    displayName = nameParts.join(' ');

    return displayName;
}

void InventoryTab::onItemSelected()
{
    QSignalBlocker listSignalBlocker(m_partsList);
    QSignalBlocker itemLevelBlocker(m_itemLevel);

    m_partsList->clear();
    m_enabledParts.clear();

    m_partName->setText({});
    m_partEffects->setText({});
    m_partNegatives->setText({});
    m_partPositives->setText({});

    QList<QListWidgetItem*> selected = m_list->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    m_selectedInventoryItem = m_list->row(selected.first());
    if (m_selectedInventoryItem >= m_savegame->inventoryItemsCount()) {
        qWarning() << "Out of bounds!";
        return;
    }

    const InventoryItem &currentInventoryItem = m_savegame->inventoryItem(m_selectedInventoryItem);
    QStringList parts;

    m_itemLevel->setValue(currentInventoryItem.level);

    QMap<QString, QString> partCategories;
    QSet<QString> categories;
    for (const ItemPart &part : ItemData::weaponParts(currentInventoryItem.objectShortName)) {
        partCategories[part.partId] = part.category;
        categories.insert(part.category);
    }


    QHash<QString, QTreeWidgetItem*> categoryItems;
    for (const QString &category : categories) {
        QTreeWidgetItem *item = new QTreeWidgetItem({category});
        m_partsList->addTopLevelItem(item);
        categoryItems[category] = item;
    }

    QStringList nameText, effectsText, negativesText, positivesText;

    const QString assetId = currentInventoryItem.data.val.split('.').last();
    if (ItemData::hasItemInfo(assetId)) {
        const ItemInfo &info = ItemData::itemInfo(assetId);
        if (!info.inventoryName.isEmpty()) {
            nameText.append(info.inventoryName);
            qDebug() << "Inventory name" << info.inventoryName;
        }
        if (!info.canDropOrSell) {
            effectsText.append(" ??? Can't be dropped or sold");
        }
        if (info.inventorySize > 1) {
            effectsText.append(QString::fromUtf8(" ??? Inventory size %1").arg(info.inventorySize));
        } else if (info.inventorySize == 0) {
            effectsText.append(" ??? Takes no space in inventory");
        }
        if (info.monetaryValue > 1) {
            effectsText.append(QString::fromUtf8(" ??? Monetary value %1").arg(info.monetaryValue));
        }
    } else {
        qWarning() << "Missing info for asset" << assetId;
    }


    for (int partIndex = 0; partIndex < currentInventoryItem.parts.count(); partIndex++) {
        const InventoryItem::Aspect &part = currentInventoryItem.parts[partIndex];

        const QString name = part.val.split('.').last();
        m_enabledParts.insert(name);


        QString category;

        if (partCategories.contains(name)) {
            category = partCategories[name];
        } else {
            category = ItemData::weaponPartType(name);
            if (category.isEmpty()) {
                qWarning() << "Unknown category for" << name;
                category = "Unknown type";
            }
            if (!categoryItems.contains(category)) {
                categoryItems[category] = new QTreeWidgetItem({category});
                m_partsList->addTopLevelItem(categoryItems[category]);
            }

            qWarning() << currentInventoryItem.name << currentInventoryItem.objectShortName << "has part" << name << "which is not in the list of parts for" << currentInventoryItem.name;
        }

        const ItemDescription description = ItemData::itemDescription(name);
        if (!description.naming.isEmpty()) {
            nameText.append(" ??? " + description.naming);
        }
        if (!description.effects.isEmpty()) {
            effectsText.append(" ??? " + description.effects);
        }
        if (!description.negatives.isEmpty()) {
            negativesText.append("??? " + description.negatives);
        }
        if (!description.positives.isEmpty()) {
            positivesText.append("??? " + description.positives);
        }
    }

    for (const QString &partId : partCategories.keys()) {
//        if (enabledParts.contains(partId)) {
//            continue;
//        }


        QTreeWidgetItem *listItem = new QTreeWidgetItem(categoryItems[partCategories[partId]], {makeNamePretty(partId)});
        listItem->setFlags(Qt::ItemIsSelectable | Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        listItem->setData(0, Qt::UserRole, partId);
        if (m_enabledParts.contains(partId)) {
            listItem->setCheckState(0, Qt::Checked);
        } else {
            listItem->setCheckState(0, Qt::Unchecked);
        }
    }
    for (QTreeWidgetItem *categoryItem : categoryItems.values()) {
        categoryItem->setExpanded(true);
    }
    positivesText.removeAll("??? DO NOT REMOVE"); // I'm very, very lazy
    positivesText.removeAll("??? -");

    m_partName->setText(nameText.join('\n'));
    m_partEffects->setText(effectsText.join('\n'));
    m_partNegatives->setText(negativesText.join('\n'));
    m_partPositives->setText(positivesText.join('\n'));

    checkValidity();

//    qDebug() << "Part count" << item.numberOfParts;
//    qDebug() << "Version" << item.version << "level" << item.level;
//    qDebug() << item.balance.val;
//    qDebug() << item.data.val;
//    qDebug() << item.manufacturer.val;
//    qDebug() << item.data.val;
//    qDebug() << item.balance.val;
}

void InventoryTab::onPartSelected()
{
    QList<QTreeWidgetItem*> selected = m_partsList->selectedItems();
    if (selected.isEmpty()) {
        return;
    }
    QString itemId = selected.first()->data(0, Qt::UserRole).toString();
    if (itemId.isEmpty()) {
        return;
    }
    const ItemDescription description = ItemData::itemDescription(itemId);
    m_partName->setText(description.naming);
    m_partEffects->setText(description.effects);
    m_partNegatives->setText(description.negatives);
    m_partPositives->setText(description.positives);

}

void InventoryTab::onPartChanged(QTreeWidgetItem *item, int column)
{
    QSignalBlocker listSignalBlocker(m_partsList);

    if (column != 0) {
        qWarning() << "Unexpected column" << column;
    }
    const QString itemId = ItemData::objectForShortName(item->data(0, Qt::UserRole).toString());
    if (itemId.isEmpty()) {
        qWarning() << "Empty part id" << item->text(0) << item;
        return;
    }
    const bool enabled = item->checkState(0) == Qt::Checked;

    const QString itemPartCategory = ItemData::partCategory(itemId);
    if (itemPartCategory.isEmpty()) {
        QMessageBox::warning(nullptr, "Invalid item", tr("Failed to find %1\nin list of items with parts.").arg(itemId));
        item->setCheckState(0, enabled ? Qt::Unchecked : Qt::Checked); // reverse
        return;
    }

    if (!enabled) {
        m_savegame->removeInventoryItemPart(m_selectedInventoryItem, item->data(0, Qt::UserRole).toString());
        m_enabledParts.remove(item->data(0, Qt::UserRole).toString());
        checkValidity();
        return;
    }

    const InventoryItem &currentInventoryItem = m_savegame->items()[m_selectedInventoryItem];
    qDebug() << itemId << "Part category" << itemPartCategory;
    InventoryItem::Aspect part = ItemData::createInventoryItemPart(currentInventoryItem, itemId);
    if (part.index <= 0) {
        QMessageBox::warning(nullptr, "Invalid item", tr("Failed to find %1\nin list of parts for item.").arg(itemId));
        item->setCheckState(0, enabled ? Qt::Unchecked : Qt::Checked); // reverse
        return;
    }
    m_savegame->addInventoryItemPart(m_selectedInventoryItem, part);
    m_enabledParts.insert(item->data(0, Qt::UserRole).toString());
//    qDebug() << "existing index" << existingPartPosition << "new part name" << part.val;
    checkValidity();
}

void InventoryTab::load()
{
    m_list->clear();
    for (const InventoryItem &item : m_savegame->items()) {
        QString rarity = item.objectShortName.split('_').last();
        m_list->addItem(tr("%1 (level %2)").arg(item.name, QString::number(item.level)));
    }
    checkValidity();
}

void InventoryTab::onItemLevelChanged()
{
    m_savegame->setItemLevel(m_selectedInventoryItem, m_itemLevel->value());
}

void InventoryTab::checkValidity()
{
    m_warningText->clear();
    m_warningText->hide();

    QString warningText;

    QHash<QString, int> maxInCategories;
    QHash<QString, int> minInCategories;
    QHash<QString, int> enabledInCategories;

    if (m_selectedInventoryItem <= 0) {
        return;
    }
    const InventoryItem &currentInventoryItem = m_savegame->inventoryItem(m_selectedInventoryItem);
    if (!currentInventoryItem.isValid()) {
        qWarning() << "Current invalid";
        return;
    }

    QSet<QString> partsToProcess = m_enabledParts;
    for (const ItemPart &part : ItemData::weaponParts(currentInventoryItem.objectShortName)) {
        if (!partsToProcess.contains(part.partId)) {
            continue;
        }
        partsToProcess.remove(part.partId);
        if (!maxInCategories.contains(part.category)) {
            maxInCategories[part.category] = part.maxParts;
        } else {
            maxInCategories[part.category] = qMax(part.maxParts, maxInCategories[part.category]);
        }

        if (!minInCategories.contains(part.category)) {
            minInCategories[part.category] = part.minParts;
        } else {
            minInCategories[part.category] = qMin(part.minParts, minInCategories[part.category]);
        }

        bool hasRequired = part.dependencies.isEmpty();
        QStringList requiredPrettyNames;
        for (const QString &required : part.dependencies) {
            if (required.isEmpty()) {
                qWarning() << "Empty dependency for" << part.partId;
                continue;
            }
            requiredPrettyNames.append(makeNamePretty(required));
            if (m_enabledParts.contains(required)) {
                hasRequired = true;
            }
        }
        if (!hasRequired) {
            warningText += tr("%1 requires one of: %2\n").arg(makeNamePretty(part.partId), makeNamePretty(requiredPrettyNames.join(", ")));
        }
        for (const QString &excluder : part.excluders) {
            if (excluder.isEmpty()) {
                qWarning() << "Empty excluder for" << part.partId;
                continue;
            }
            if (m_enabledParts.contains(excluder)) {
                warningText += tr("%1 can't be combined with %2\n").arg(makeNamePretty(part.partId), makeNamePretty(excluder));
            }
        }
        enabledInCategories[part.category]++;
    }
    if (!partsToProcess.isEmpty()) {
        warningText += tr("%1 unknown parts for current item\n").arg(partsToProcess.count());
        qDebug() << partsToProcess;
    }
    for (const QString &category : enabledInCategories.keys()) {
        const int count = enabledInCategories[category];
        if (count < minInCategories[category]) {
            warningText += tr("Category %1 requires at least %2 parts, only has %3\n").arg(category).arg(minInCategories[category]).arg(count);
        }
        if (count > maxInCategories[category]) {
            warningText += tr("Category %1 can only have %2 parts, has %3\n").arg(category).arg(maxInCategories[category]).arg(count);
        }
    }
    if (!warningText.isEmpty()) {
        m_warningText->setText(warningText);
        m_warningText->show();
    }
}
