//! `Metadata` QObject — wraps `core::metadata::MetadataManager`.
//! Phase 2 stub.

#[cxx_qt::bridge]
pub mod qobject {
    unsafe extern "C++" {
        include!("cxx-qt-lib/qstring.h");
        type QString = cxx_qt_lib::QString;
    }

    extern "RustQt" {
        #[qobject]
        #[qml_element]
        #[qml_singleton]
        type Metadata = super::MetadataRust;

        #[qinvokable]
        fn placeholder(self: &Metadata) -> QString;
    }
}

#[derive(Default)]
pub struct MetadataRust;

impl qobject::Metadata {
    fn placeholder(self: &qobject::Metadata) -> cxx_qt_lib::QString {
        cxx_qt_lib::QString::from("Metadata stub — Phase 3")
    }
}
