#include "Savegame.h"

#include "OakSave.pb.h"

#include "obfuscation.h"

#include <QBitArray>
#include <QFile>
#include <QMessageBox>
#include <QtEndian> // all the qFromLittleEndian is valid for the PC saves at least
#include <QDebug>
#include <deque>

extern "C" {
#include <zlib.h> // Because I need crc32, and everyone has zlib
}

//#include <bitset> // More stuff that we want than QBitSet (like shifting) fuck std

// Idea stolen wholesale from CJ
// I'm not in the mood to write a long switch-case-shift-bits-thing
struct BitParser
{
    BitParser(const QByteArray &data)
    {
        QBitArray bits = QBitArray::fromBits(data.data(), data.size() * 8);
        for (int i=0; i < bits.size(); i++) {
            m_bits.append(bits[i] ? '1' : '0');
        }
    }

    int eat(const int count) {
        if (count <= 0) {
            return 0;
        }

        QByteArray toEat = m_bits.mid(0, count);
        m_bits = m_bits.mid(count);
        std::reverse(toEat.begin(), toEat.end()); // idk lol I don't know computer

        bool ok; // lol as if
        return toEat.toInt(&ok, 2);
    }
private:
    QByteArray m_bits;
};

Savegame::Savegame(QObject *parent) :
    QObject(parent)
{
    m_character = std::make_unique<OakSave::Character>();
}

Savegame::~Savegame()
{ // can't be inline or default, because unique_ptr in gcc is short-bus special
}

static QByteArray decode(const QByteArray &input)
{
    if (input.size() < 6) {
        qWarning() << "Invalid serial";
        return {};
    }
    if (input[0] != 3) {
        qWarning() << "Invalid serial, doesn't start with 3";
        return {};
    }

    const int32_t seed = qFromBigEndian<int32_t>(input.data() + 1);

    QByteArray data = input.mid(5); // 1 first byte is 3, 4 bytes is int seed

    if (seed != 0) {
        uint32_t key = (seed >> 5) & 0xFFFFFFFF;
        for (char &c : data) {
            key = (key * 0x10A860C1ull) % 0xFFFFFFFB;
            c ^= key;
        }
        const int steps = (seed & 0x1f) % data.size();
        std::rotate(data.rbegin(), data.rbegin() + steps, data.rend());
    } else {
        qWarning() << "0 seed?";
    }

    QByteArray toChecksum = input.mid(0, 5) + "\xff\xff" + data.mid(2);
    // Use zlib's crc32 because fuck if I want another dependency
    uLong crc = crc32(0L, Z_NULL, 0);
    crc = crc32(crc, (uint8_t*)toChecksum.data(), toChecksum.size());
    const uint16_t computedChecksum = (crc >> 16) ^ crc;
    const uint16_t checksum = qFromBigEndian<uint16_t>(data.data());
    if (computedChecksum != checksum) {
        qWarning() << "Checksum mismatch" << computedChecksum << "expected" << checksum;
        return {};
    }

    return data.mid(2);
}

// could be simpler and more efficient and who uses powerpc these days, but meh
template <typename T>
static bool readInt(T *output, QIODevice *input)
{
    char data[sizeof(T)];
    if (input->read(data, sizeof(data)) != qint64(sizeof(data))) {
        return false;
    }
    *output = qFromLittleEndian<T>(data);
    return true;
}

template <typename T>
static bool writeInt(const T input, QIODevice *output)
{
    char data[sizeof(T)];
    qToLittleEndian(input, data);
    const qint64 written = output->write(data, sizeof(data));
    if (written != qint64(sizeof(data))) {
        qWarning() << "short writE" << written << sizeof(data);
        return false;
    }
    return true;
}

static bool readString(QString *output, QIODevice *input)
{
    int stringLength;
    if (!readInt(&stringLength, input) || stringLength < 0) {
        return false;
    }

    // just an empty string, because it's supposed to be 0 terminated
    if (stringLength <= 1) {
        return true;
    }
    *output = QString::fromUtf8(input->read(stringLength), stringLength - 1); // Supposed to be \0-terminated, but just in case
    return true;
}

static bool writeString(const QString &input, QIODevice *output)
{
    const QByteArray utf8 = input.toUtf8() + '\0';
    const int stringLength = utf8.length();
    if (!writeInt(stringLength, output) || stringLength < 0) {
        return false;
    }

    output->write(utf8);

    return true;
}

bool Savegame::load(const QString &filePath)
{
    m_header = {};

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(nullptr, "Failed to open file", file.errorString());
        return false;
    }

    const QByteArray fileMagic = file.read(4);
    if (fileMagic != "GVAS") {
        QMessageBox::warning(nullptr, "Invalid header", "Invalid file, starts with:\n" + fileMagic.toHex() + "'.");
        return false;
    }
    bool couldReadHeader =
            readInt(&m_header.savegameVersion, &file) &&
            readInt(&m_header.packageVersion, &file) &&
            readInt(&m_header.engineMajorVersion, &file) &&
            readInt(&m_header.engineMinorVersion, &file) &&
            readInt(&m_header.enginePatchVersion, &file) &&
            readInt(&m_header.engineBuild, &file) &&
            readString(&m_header.buildId, &file) &&
            readInt(&m_header.customFormatVersion, &file) &&
            readInt(&m_header.customFormatCount, &file);

    if (!couldReadHeader) {
        QMessageBox::warning(nullptr, "Invalid header", "Invalid file, failed to read header.\n" + file.errorString());
        return false;
    }
    if (m_header.customFormatCount > 1000) { // idk, just sanity
        QMessageBox::warning(nullptr, "Invalid header", "Invalid file, too many custom formats: " + QString::number(m_header.customFormatCount));
        return false;
    }
    qDebug() << "Custom formats" << m_header.customFormatCount;

    m_header.customFormats.resize(m_header.customFormatCount);

    for (Header::CustomFormat &format : m_header.customFormats) {
        format.id = QUuid::fromRfc4122(file.read(16));
        if (format.id.isNull()) {
            QMessageBox::warning(nullptr, "Invalid header", "Invalid custom format description id");
            return false;
        }
        if (!readInt(&format.entry, &file)) {
            QMessageBox::warning(nullptr, "Invalid header", "Invalid file, failed to read custom format entry index.\n" + file.errorString());
            return false;
        }
        qDebug() << "Format" << format.id << format.entry;
    }
    if (!readString(&m_header.savegameType, &file)) {
        QMessageBox::warning(nullptr, "Invalid header", "Invalid file, failed to read savegame type.\n" + file.errorString());
        return false;
    }
    if (!readInt(&m_header.dataLength, &file)) {
            QMessageBox::warning(nullptr, "Invalid header", "Failed to read data length");
            return false;
    }

    qDebug() << "Savegame version" << m_header.savegameType << m_header.savegameVersion;
    qDebug() << "Package version" << m_header.packageVersion;
    qDebug() << "Build id:" << m_header.buildId;
    qDebug() << "Engine version" << m_header.engineMajorVersion << m_header.engineMinorVersion << m_header.enginePatchVersion << m_header.engineBuild;
    qDebug() << "Custom format version" << m_header.customFormatVersion;


    QByteArray data = file.readAll();
    if (data.size() != m_header.dataLength) { // yeah yeah, padding, but it needs to be significantly larger so whatever
        QMessageBox::warning(nullptr, "Failed to read from file", "Wrong amount of data available, expected " + QString::number(m_header.dataLength) + ", but got " + QString::number(data.size()));
        return false;
    }

    char *dataRaw = data.data();
    for (int i=data.size() - 1; i >= 0; i--) {
        // I want a prize for ugly code
        // Premature optimization^Wobfuscation (it's probably not faster than doing it the pretty way)
        dataRaw[i] ^= (i < int(sizeof(obfuscation::prefixMask)) ? obfuscation::prefixMask[i] : dataRaw[i - sizeof(obfuscation::prefixMask)])
            ^ obfuscation::xorMask[i % sizeof(obfuscation::xorMask)];
    }

    if (!m_character->ParseFromArray(dataRaw, data.size())) {
        // protobuf never gives us anything, but whatever
        QMessageBox::warning(nullptr, "Invalid file", "Failed to parse file contents:\n" + QString::fromStdString(m_character->InitializationErrorString()));
        return false;
    }

    if (!m_character->IsInitialized()) {
        return false;
    }

    qDebug() << "Items:" << m_character->inventory_items_size();
//    int maxBits = 0;
//    if (m_character->inventory_items_size() > 0) {
    for (int i=0; i<m_character->inventory_items_size(); i++) {
        const ::OakSave::OakInventoryItemSaveGameData& entry = m_character->inventory_items(i);
        QByteArray serial = decode(QByteArray::fromStdString(entry.item_serial_number()));
        if (serial.isEmpty()) {
            qWarning() << "Couldn't parse item";
            return false;
        }

        BitParser bits(serial);
        if (bits.eat(8) != 128) {
            qWarning() << "Invalid start";
            return false;
        }
        int version = bits.eat(7);
        qDebug() << "Version" << version;
    }
//    qDebug() << "Max bits:" << maxBits;


    const QString backupFilename = file.fileName() + ".backup";
    QFile::remove(backupFilename);
    file.copy(backupFilename);

    emit nameChanged(characterName());
    emit xpChanged(xp());
    emit levelChanged(level());
    emit moneyChanged(money());
    emit eridiumChanged(eridium());
    return true;
}

bool Savegame::save(const QString filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        QMessageBox::warning(nullptr, "Failed to open file", file.errorString());
        return false;
    }
    file.write("GVAS");
    bool couldWriteHeader =
            writeInt(m_header.savegameVersion, &file) &&
            writeInt(m_header.packageVersion, &file) &&
            writeInt(m_header.engineMajorVersion, &file) &&
            writeInt(m_header.engineMinorVersion, &file) &&
            writeInt(m_header.enginePatchVersion, &file) &&
            writeInt(m_header.engineBuild, &file) &&
            writeString(m_header.buildId, &file) &&
            writeInt(m_header.customFormatVersion, &file) &&
            writeInt(m_header.customFormatCount, &file);

    if (!couldWriteHeader) {
        QMessageBox::warning(nullptr, "Invalid header", "Failed to write header to file:\n" + file.errorString());
        return false;
    }

    for (const Header::CustomFormat &format : m_header.customFormats) {
        file.write(format.id.toRfc4122());
        writeInt(format.entry, &file);
    }
    writeString(m_header.savegameType, &file);

    QByteArray data = QByteArray::fromStdString(m_character->SerializeAsString());

    char *dataRaw = data.data();
    for (int i=0; i<data.size(); i++) {
        // I want a prize for ugly code
        // Premature optimization^Wobfuscation (it's probably not faster than doing it the pretty way)
        dataRaw[i] ^= (i < int(sizeof(obfuscation::prefixMask)) ? obfuscation::prefixMask[i] : dataRaw[i - sizeof(obfuscation::prefixMask)])
            ^ obfuscation::xorMask[i % sizeof(obfuscation::xorMask)];
    }
    qDebug() << file.pos();

    writeInt(data.length(), &file);
    file.write(data);

    return true;
}

QString Savegame::characterName() const
{
    return QString::fromStdString(m_character->preferred_character_name());
}

void Savegame::setCharacterName(const QString &name)
{
    m_character->set_preferred_character_name(name.toStdString());
}

int Savegame::xp() const
{
    return m_character->experience_points();
}

void Savegame::setXp(const int newXp)
{
    if (newXp == xp()) {
        return;
    }
    const int oldLevel = level();

    m_character->set_experience_points(newXp);

    if (oldLevel != level()) {
        emit levelChanged(level());
    }
}

int Savegame::level() const
{
    const int currentXp = xp();
    int level = 0;
    for (const int requiredXp : Constants::requiredXp) {
        if (currentXp >= requiredXp) {
            level++;
        } else {
            return level;
        }
    }

    qWarning() << "Invalid amount of xp" << currentXp << ", can't get level";
    return level;
}

void Savegame::setLevel(const int newLevel)
{
    if (newLevel > Constants::maxLevel) {
        qWarning() << "Invalid new level" << newLevel;
        return;
    }

    if (newLevel == level()) {
        return;
    }
    const int requiredXp = Constants::requiredXp[newLevel-1];
    if (requiredXp != xp()) {
        m_character->set_experience_points(requiredXp);
        emit xpChanged(requiredXp);
    }
}

int Savegame::money() const
{
    return currencyAmount(Constants::Currency::Money);
}

void Savegame::setMoney(const int amount)
{
    if (amount == money()) {
        return;
    }

    setCurrency(Constants::Currency::Money, amount);
    emit moneyChanged(amount);
}

int Savegame::eridium() const
{
    return currencyAmount(Constants::Currency::Eridium);
}

void Savegame::setEridium(const int amount)
{
    if (amount == eridium()) {
        return;
    }

    setCurrency(Constants::Currency::Eridium, amount);
    emit eridiumChanged(amount);
}

int Savegame::currencyAmount(const Constants::Currency currency) const
{
    for (const OakSave::InventoryCategorySaveData &item : m_character->inventory_category_list()) {
        if (Constants::currencyByHash(item.base_category_definition_hash()) != currency) {
            continue;
        }
        return item.quantity();
    }
    qWarning() << "Failed to find category" << currency;

    return 0;

}

void Savegame::setCurrency(const Constants::Currency currency, const int amount)
{
    for (OakSave::InventoryCategorySaveData &item : *m_character->mutable_inventory_category_list()) {
        if (Constants::currencyByHash(item.base_category_definition_hash()) != currency) {
            continue;
        }
        item.set_quantity(amount);
    }

    qDebug() << "Failed to find category, adding new";
    int hash = Constants::hashByCurrency(currency);
    if (hash == -1) {
        qWarning() << "Failed to find hash for" << currency;
        return;
    }
    OakSave::InventoryCategorySaveData newItem;
    newItem.set_base_category_definition_hash(hash);
    newItem.set_quantity(amount);
    m_character->mutable_inventory_category_list()->Add(std::move(newItem));
}