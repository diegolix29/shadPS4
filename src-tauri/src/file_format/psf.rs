use crate::file_format::psf::file_format::*;
use num_traits::FromPrimitive;
use serde::{Deserialize, Serialize};
use std::collections::HashMap;
use std::fs::{metadata, File};
use std::io;
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom};
use std::path::Path;
use std::string::FromUtf8Error;
use std::time::SystemTime;
use zerocopy::FromBytes;

/// This is a direct conversion from
/// https://github.com/shadps4-emu/shadPS4/blob/b41664ac616894686e072ede61c609b422d79ed4/src/core/file_format/psf.h

mod file_format {
    use num_derive::FromPrimitive;
    use static_assertions::assert_eq_size;
    use zerocopy::big_endian;
    use zerocopy_derive::{FromBytes, Immutable, KnownLayout};

    pub(super) const PSF_MAGIC: u32 = 0x00505346;
    pub(super) const PSF_VERSION_1_1: u32 = 0x00000101;
    pub(super) const PSF_VERSION_1_0: u32 = 0x00000100;

    #[repr(C)]
    #[derive(FromBytes, KnownLayout, Immutable)]
    pub(super) struct FileHeader {
        pub magic: big_endian::U32,
        pub version: u32,
        pub key_table_offset: u32,
        pub data_table_offset: u32,
        pub index_table_entries: u32,
    }
    assert_eq_size!(FileHeader, [u8; 0x14]);

    #[repr(C)]
    #[derive(FromBytes, KnownLayout, Immutable)]
    pub(super) struct RawEntry {
        pub key_offset: u16,
        pub param_fmt: big_endian::U16,
        pub param_len: u32,
        pub param_max_len: u32,
        pub data_offset: u32,
    }
    assert_eq_size!(RawEntry, [u8; 0x10]);

    #[repr(u16)]
    #[derive(FromPrimitive)]
    pub(super) enum EntryFmt {
        Binary = 0x0400,  // Binary data
        Text = 0x0402,    // String in UTF-8 format and NULL terminated
        Integer = 0x0404, // Signed 32-bit integer
    }
}

#[derive(Debug, Serialize, Deserialize)]
pub enum Value {
    Binary(Vec<u8>),
    Text(String),
    Integer(i32),
}

#[derive(thiserror::Error, Debug)]
pub enum Error {
    #[error("io error: {0}")]
    IO(#[from] io::Error),
    #[error("invalid PSF header magic code: {0:X}")]
    InvalidMagicCode(u32),
    #[error("unsupported PSF header version: {0:X}")]
    InvalidVersion(u32),
    #[error("invalid entry format: {0:X}")]
    InvalidEntryFormat(u16),
    #[error("invalid text entry: {0}")]
    InvalidTextEntryUtf(#[from] FromUtf8Error),
    #[error("invalid integer entry size: {0} != {1}")]
    InvalidIntEntry(u32, usize),
}

#[derive(Debug, Serialize, Deserialize)]
pub struct PSF {
    last_write: SystemTime,
    entries: HashMap<String, Value>,
}

impl Default for PSF {
    fn default() -> Self {
        Self {
            last_write: SystemTime::UNIX_EPOCH,
            entries: HashMap::new(),
        }
    }
}

impl PSF {
    pub fn new() -> Self {
        Default::default()
    }

    #[allow(dead_code)]
    pub fn open<P: AsRef<Path>>(path: P) -> Result<Self, Error> {
        let metadata = metadata(&path)?;
        let f = File::open(&path)?;
        let mut s = Self::default();
        s.last_write = metadata.modified()?;
        s.read(f)?;
        Ok(s)
    }

    fn read<R: Read + Seek>(&mut self, mut r: R) -> Result<(), Error> {
        r.seek(SeekFrom::Start(0))?;
        let header = FileHeader::read_from_io(&mut r)?;
        if header.magic != PSF_MAGIC {
            return Err(Error::InvalidMagicCode(header.magic.get()));
        }
        if header.version != PSF_VERSION_1_0 && header.version != PSF_VERSION_1_1 {
            return Err(Error::InvalidVersion(header.version));
        }

        let mut entries = Vec::<RawEntry>::new();
        entries.reserve_exact(header.index_table_entries as usize);
        for _ in 0..header.index_table_entries {
            entries.push(RawEntry::read_from_io(&mut r)?);
        }

        self.entries = entries
            .iter()
            .map(|e| -> Result<(String, Value), Error> {
                r.seek(SeekFrom::Start(
                    (header.key_table_offset + e.key_offset as u32) as u64,
                ))?;

                let key = {
                    let mut data = Vec::new();
                    let mut r = BufReader::new(&mut r);
                    r.read_until(b'\0', &mut data)?;
                    if data.last() == Some(&0) {
                        data.pop();
                    }
                    String::from_utf8(data)?
                };

                r.seek(SeekFrom::Start(
                    (header.data_table_offset + e.data_offset) as u64,
                ))?;

                let Some(fmt) = EntryFmt::from_u16(e.param_fmt.get()) else {
                    return Err(Error::InvalidEntryFormat(e.param_fmt.get()));
                };

                let value: Value = match fmt {
                    EntryFmt::Binary => {
                        let mut buf = vec![0u8; e.param_len as usize];
                        r.read_exact(&mut buf)?;
                        Value::Binary(buf)
                    }
                    EntryFmt::Text => {
                        let mut data = Vec::new();
                        let mut r = BufReader::new(&mut r);
                        r.read_until(b'\0', &mut data)?;
                        if data.last() == Some(&0) {
                            data.pop();
                        }
                        Value::Text(String::from_utf8(data)?)
                    }
                    EntryFmt::Integer => {
                        if e.param_len as usize != size_of::<i32>() {
                            return Err(Error::InvalidIntEntry(e.param_len, size_of::<i32>()));
                        }
                        let mut data = [0; size_of::<i32>()];
                        r.read_exact(&mut data)?;
                        Value::Integer(i32::from_le_bytes(data))
                    }
                };

                Ok((key, value))
            })
            .collect::<Result<HashMap<String, Value>, Error>>()?;

        Ok(())
    }
}

pub mod js {
    use crate::file_format::psf::PSF;
    use anyhow_tauri::IntoTAResult;
    use log::error;
    use std::fs::metadata;
    use tauri::AppHandle;
    use tauri_plugin_fs::{FsExt, OpenOptions, SafeFilePath};

    #[tauri::command]
    pub fn read_psf(app: AppHandle, path: SafeFilePath) -> anyhow_tauri::TAResult<PSF> {
        _read_psf(app, path).into_ta_result()
    }

    fn _read_psf(app: AppHandle, path: SafeFilePath) -> anyhow::Result<PSF> {
        let path = path.into_path()?;
        let mut f = app
            .fs()
            .open(&path, OpenOptions::new().read(true).clone())?;

        let mut psf = PSF::new();
        psf.last_write = metadata(&path)?.modified()?;
        psf.read(&mut f)
            .inspect_err(|e| error!("error reading psf file: {}", e))?;

        Ok(psf)
    }
}
