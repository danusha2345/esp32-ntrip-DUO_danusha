# GitHub Actions CI/CD Pipeline

Этот репозиторий использует GitHub Actions для автоматической сборки и релиза прошивок ESP32 NTRIP DUO.

## 🔄 Workflows

### 1. Build Test (`build-test.yml`)
**Триггеры:** Push в master/main/develop, Pull Request в master/main

**Назначение:** Проверка сборки при каждом коммите

**Функции:**
- ✅ Сборка для всех поддерживаемых платформ (ESP32, ESP32-C3, ESP32-S3, ESP32-C6)
- 📊 Проверка размера прошивки  
- 🚨 Предупреждения при превышении лимитов
- 📋 Загрузка логов при ошибках сборки
- ✅ Итоговый статус всех сборок

**Пример запуска:**
```
✅ ESP32: Build successful (951 KB)
✅ ESP32-C3: Build successful (932 KB)
✅ ESP32-S3: Build successful (967 KB)  
✅ ESP32-C6: Build successful (943 KB)
```

### 2. Build and Release (`build-release.yml`) 
**Триггеры:** Git теги `v*`, ручной запуск

**Назначение:** Автоматическое создание релизов с прошивками

**Функции:**
- 🏗️ Сборка прошивок для всех платформ
- 📦 Создание архивов с документацией
- 🚀 Автоматическая публикация релиза
- 📝 Генерация release notes
- 💾 Прикрепление бинарных файлов

**Создаваемые файлы:**
```
esp32-ntrip-duo-esp32-YYYYMMDD.tar.gz
esp32-ntrip-duo-esp32c3-YYYYMMDD.tar.gz
├── bootloader.bin
├── partition-table.bin  
├── <target>-ntrip-duo.bin
├── www.bin
├── <target>-ntrip-duo.elf
├── <target>-ntrip-duo-merged.bin
├── download.conf (Flash Download Tool)
└── README.md
```

## 🚀 Как создать релиз

### Автоматический релиз (рекомендуется)
```bash
# Создать тег с версией
jj tag set v2026.08.16 -r @-

# Отправить тег в репозиторий  
jj git push --tag v2026.08.16

# GitHub Actions автоматически:
# 1. Соберёт прошивки для всех платформ
# 2. Создаст архивы с документацией
# 3. Опубликует релиз с файлами
```

### Ручной запуск релиза
1. Перейти в **Actions** → **Build and Release**
2. Нажать **Run workflow**
3. Выбрать опции и запустить
4. Дождаться завершения сборки

## 📋 Структура релиза

Каждый релиз содержит:

### Для каждой платформы:
- **Прошивки:** `.bin` файлы для прошивки
- **Отладка:** `.elf` файл с символами
- **Скрипты:** Готовые скрипты для прошивки
- **Конфиг:** Настройки для ESP32 Flash Download Tool
- **Документация:** README с инструкциями

### Общие файлы:
- **README.md**: Общее описание релиза
- **CHANGELOG**: Список изменений
- **Архивы**: Сжатые пакеты для скачивания

## 🛠️ Локальная сборка

Для локальной сборки всех вариантов:

```bash
# Подготовить ESP-IDF окружение
export IDF_TOOLS_PATH=/home/danik/storage/espressif
source /home/danik/storage/esp/esp-idf-v5.4.4/export.sh

# Запустить сборку всех платформ
./build_all.sh

# Результат в папке build_output/
```

## 📊 Мониторинг сборок

### Статус badges
- ![Build Test](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/actions/workflows/build-test.yml/badge.svg)
- ![Build Release](https://github.com/danusha2345/esp32-ntrip-DUO_danusha/actions/workflows/build-release.yml/badge.svg)

### Проверка статуса
- **Actions tab**: Текущие и прошлые сборки
- **Commit indicators**: ✅/❌ рядом с коммитами
- **Pull Request checks**: Автоматическая проверка PR

## ⚙️ Конфигурация

### Секреты (не требуются)
Workflows используют только стандартный `GITHUB_TOKEN`

### Настройки
- **ESP-IDF версия**: v5.4.4
- **Целевые платформы**: esp32, esp32c3, esp32s3, esp32c6
- **Режим сборки**: Release оптимизация

### Кастомизация
Для изменения настроек отредактируйте `.github/workflows/*.yml`:

```yaml
# Изменить версию ESP-IDF
container: espressif/idf:v5.4.4

# Добавить новую платформу
matrix:
  target: [esp32, esp32c3, esp32s3, esp32c6, esp32h2]

# Изменить триггеры
on:
  push:
    branches: [master, develop, feature/*]
```

## 🐛 Устранение неполадок

### Сборка не запускается
- Проверить триггеры в workflow файле
- Убедиться что push попадает в нужную ветку
- Проверить синтаксис YAML файлов

### Ошибки сборки
- Посмотреть логи в Actions tab
- Скачать artifacts с ошибками
- Проверить совместимость с новой версией ESP-IDF

### Релиз не создаётся
- Убедиться что тег имеет формат `v*`  
- Проверить права доступа к репозиторию
- Проверить статус workflow в Actions

## 📈 Оптимизация

### Ускорение сборки
- Кеширование зависимостей ESP-IDF
- Параллельная сборка платформ
- Оптимизация размера контейнера

### Экономия ресурсов
- Сборка только при изменении кода
- Условные сборки для разных веток
- Очистка старых artifacts

## 🔗 Полезные ссылки

- [GitHub Actions Documentation](https://docs.github.com/en/actions)
- [ESP-IDF CI Action](https://github.com/espressif/esp-idf-ci-action)
- [ESP32 Flash Download Tool](https://www.espressif.com/en/support/download/other-tools)
