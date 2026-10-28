#pragma once

#include <array>
#include <cstddef>

namespace vpad
{
	// Rebase continuous body-axis gyro angles when the title sets its origin.
	// They can span multiple turns; do not wrap their differences as Euler angles.
	class GyroAngleReference
	{
	public:
		void SetAngle(const std::array<float, 3>& angle)
		{
			m_angle = angle;
			m_isSet = true;
		}

		std::array<float, 3> Update(const std::array<float, 3>& sample)
		{
			if (m_isSet && m_hasSample)
			{
				for (std::size_t axis = 0; axis < m_angle.size(); ++axis)
					m_angle[axis] += sample[axis] - m_lastSample[axis];
			}
			m_lastSample = sample;
			m_hasSample = true;
			return m_isSet ? m_angle : sample;
		}

	private:
		std::array<float, 3> m_angle{};
		std::array<float, 3> m_lastSample{};
		bool m_isSet = false;
		bool m_hasSample = false;
	};

	class GyroDirectionReference
	{
	public:
		using Matrix = std::array<float, 9>;

		void SetDirection(const Matrix& direction)
		{
			m_direction = direction;
			m_isSet = true;
			if (m_hasSample)
				UpdateCorrection();
		}

		Matrix Update(const Matrix& sample)
		{
			if (m_isSet && !m_hasSample)
			{
				m_lastSample = sample;
				UpdateCorrection();
			}
			m_lastSample = sample;
			m_hasSample = true;
			return m_isSet ? Multiply(sample, m_correction) : sample;
		}

	private:
		static Matrix Multiply(const Matrix& left, const Matrix& right)
		{
			Matrix result{};
			for (std::size_t row = 0; row < 3; ++row)
				for (std::size_t column = 0; column < 3; ++column)
					for (std::size_t axis = 0; axis < 3; ++axis)
						result[row * 3 + column] += left[row * 3 + axis] * right[axis * 3 + column];
			return result;
		}

		void UpdateCorrection()
		{
			// VPAD stores the body basis vectors as rows. Transform their world
			// coordinates on the right; transpose inverts the orthonormal host basis.
			// Map the pose at calibration to the game's requested basis while
			// retaining subsequent rotation instead of freezing its direction.
			Matrix inverse{};
			for (std::size_t row = 0; row < 3; ++row)
				for (std::size_t column = 0; column < 3; ++column)
					inverse[row * 3 + column] = m_lastSample[column * 3 + row];
			m_correction = Multiply(inverse, m_direction);
		}

		Matrix m_direction{};
		Matrix m_correction{};
		Matrix m_lastSample{};
		bool m_isSet = false;
		bool m_hasSample = false;
	};
}
