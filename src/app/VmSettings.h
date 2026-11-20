#pragma once

#include <QString>
#include <QStringList>

// What VM Settings shows and changes, from any hypervisor
struct VmSettings {
    int cpus = 1;
    int memoryMiB = 0;
    bool cpusFixed = false;   // set elsewhere in its definition: shown, not offered
    bool memoryFixed = false;
    bool hasCd = false;
    QString iso;              // empty: no disc
    bool cdFirst = false;     // boots from the CD/DVD before the disks

    // the host's, offered only
    int maxCpus = 1;
    int maxMemoryMiB = 0;
    QStringList isos;
    bool localFiles = false;  // the host is this computer: Browse… picks its files
    bool running = false;
};
