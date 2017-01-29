if(NOT USE_QT5)
  find_package(qt4-mimetypes REQUIRED)
  include_directories(${QT4-MIMETYPES_INCLUDE_DIRS}/qt4-mimetypes)
  get_property(QT4-MIMETYPES_LIBRARY_LOCATION TARGET ${QT4-MIMETYPES_LIBRARIES} PROPERTY LOCATION)
  get_filename_component(QT4-MIMETYPES_LIB_DIR "${QT4-MIMETYPES_LIBRARY_LOCATION}" PATH)
endif()
