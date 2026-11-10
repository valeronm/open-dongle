use anyhow::{Result, ensure};

const BLOCK: usize = 512;
const PAYLOAD_MAX: usize = 476;
const MAGIC_START0: u32 = 0x0A32_4655;
const MAGIC_START1: u32 = 0x9E5D_5157;
const MAGIC_END: u32 = 0x0AB1_6F30;
const FLAG_FAMILY_ID: u32 = 0x2000;
const NRF52840_FAMILY: u32 = 0xADA5_2840;
/// The bootloader puts the application right after S140 6.x, which ends here.
const APP_START: u32 = 0x26000;

/// The image a `.uf2` file holds, checked to be one gap-free nRF52840 range starting at
/// `APP_START`. The last block's padding stays in the image, since UF2 does not mark it.
pub fn image(file: &[u8]) -> Result<Vec<u8>> {
    ensure!(
        !file.is_empty() && file.len().is_multiple_of(BLOCK),
        "not a UF2 file: its size is not a whole number of 512-byte blocks"
    );
    let mut image = Vec::new();
    for (i, block) in file.as_chunks::<BLOCK>().0.iter().enumerate() {
        let word = |at: usize| u32::from_le_bytes(block[at..at + 4].try_into().unwrap());
        ensure!(
            word(0) == MAGIC_START0 && word(4) == MAGIC_START1 && word(508) == MAGIC_END,
            "not a UF2 file: block {i} lacks the UF2 markers"
        );
        let (addr, size) = (word(12), word(16) as usize);
        ensure!(
            size <= PAYLOAD_MAX,
            "not a UF2 file: block {i} claims {size} bytes"
        );
        ensure!(
            word(8) & FLAG_FAMILY_ID != 0 && word(28) == NRF52840_FAMILY,
            "the firmware is not for the nRF52840 (block {i} has family {:#x})",
            word(28)
        );
        let expected = APP_START + image.len() as u32;
        ensure!(
            addr == expected,
            "the firmware does not fit this bootloader: block {i} is at {addr:#x}, expected {expected:#x} \
             (the application starts at {APP_START:#x})"
        );
        image.extend_from_slice(&block[32..32 + size]);
    }
    ensure!(!image.is_empty(), "the UF2 file holds no firmware");
    Ok(image)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn block(addr: u32, data: &[u8], family: u32) -> Vec<u8> {
        let mut b = vec![0u8; BLOCK];
        let words = [
            MAGIC_START0,
            MAGIC_START1,
            FLAG_FAMILY_ID,
            addr,
            data.len() as u32,
            0,
            1,
            family,
        ];
        for (i, w) in words.iter().enumerate() {
            b[i * 4..i * 4 + 4].copy_from_slice(&w.to_le_bytes());
        }
        b[32..32 + data.len()].copy_from_slice(data);
        b[508..].copy_from_slice(&MAGIC_END.to_le_bytes());
        b
    }

    #[test]
    fn consecutive_blocks_join_into_one_image() {
        let mut file = block(APP_START, &[1; 256], NRF52840_FAMILY);
        file.extend(block(APP_START + 256, &[2; 256], NRF52840_FAMILY));
        let image = image(&file).unwrap();
        assert_eq!(image.len(), 512);
        assert_eq!((image[0], image[511]), (1, 2));
    }

    #[test]
    fn a_block_for_another_chip_is_refused() {
        let err = image(&block(APP_START, &[1; 256], 0x1c5f_21b0)).unwrap_err();
        assert!(err.to_string().contains("nRF52840"), "{err}");
    }

    #[test]
    fn an_image_starting_elsewhere_is_refused() {
        let err = image(&block(0x27000, &[1; 256], NRF52840_FAMILY)).unwrap_err();
        assert!(err.to_string().contains("0x27000"), "{err}");
    }

    #[test]
    fn a_gap_between_blocks_is_refused() {
        let mut file = block(APP_START, &[1; 256], NRF52840_FAMILY);
        file.extend(block(APP_START + 512, &[2; 256], NRF52840_FAMILY));
        let err = image(&file).unwrap_err();
        assert!(err.to_string().contains("0x26200"), "{err}");
    }

    #[test]
    fn a_file_that_is_not_uf2_is_refused() {
        assert!(image(&[0u8; 100]).is_err());
        assert!(image(&[0u8; BLOCK]).is_err());
        assert!(image(&[]).is_err());
    }

    #[test]
    fn a_block_claiming_more_than_it_can_hold_is_refused() {
        let mut file = block(APP_START, &[1; 256], NRF52840_FAMILY);
        file[16..20].copy_from_slice(&477u32.to_le_bytes());
        let err = image(&file).unwrap_err();
        assert!(err.to_string().contains("claims"), "{err}");
    }

    #[test]
    fn a_block_without_the_family_flag_is_refused() {
        let mut file = block(APP_START, &[1; 256], NRF52840_FAMILY);
        file[8..12].copy_from_slice(&0u32.to_le_bytes());
        let err = image(&file).unwrap_err();
        assert!(err.to_string().contains("nRF52840"), "{err}");
    }

    #[test]
    fn a_file_without_data_is_refused() {
        let err = image(&block(APP_START, &[], NRF52840_FAMILY)).unwrap_err();
        assert!(err.to_string().contains("no firmware"), "{err}");
    }
}
